/*
  Kjetil Matheussen, 2005-2013.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "das_config.h"

#define _GNU_SOURCE 1 // For asprintf

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sndfile.h>
#include <pthread.h>
#include <math.h>
#include <stdarg.h>

#include <termios.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/resource.h>

#include <jack/jack.h>

#include <libgen.h>
#include <sys/wait.h>

#if HAVE_LAME
#include <lame/lame.h>
#endif
#if HAVE_LIBLO
#include <lo/lo.h>
/* defined in osc.c */
int  init_osc(int osc_port);
void shutdown_osc(void);
#endif

#include "jack_capture.h"
#include "sema.h"
#include "atomic.h"
#include "vringbuffer.h"

#define JC_MAX(a,b) (((a)>(b))?(a):(b))
#define JC_MIN(a,b) (((a)<(b))?(a):(b))

#define ALIGN_UP(value, alignment) (((uintptr_t)value + alignment - 1) & -alignment)

// Using double because double should always be very large.
#define ALIGN_UP_DOUBLE(p) ALIGN_UP(p, sizeof(double)) 

/* Arguments and their default values */
#define DEFAULT_MIN_BUFFER_TIME 4
#define DEFAULT_MIN_MP3_BUFFER_TIME 8
static bool DEBUG = 0;
static float min_buffer_time = -1.0f;
static float max_buffer_time = 40.0f;
static jack_client_t * client=NULL;
#define DEFAULT_NUM_CHANNELS 2
#define DEFAULT_NUM_CHANNELS_SDS 1
static int num_channels=-1;
static int bitdepth=0;
static char *base_filename=NULL;
static char *filename=NULL;
static bool use_vu=true;
static bool use_meterbridge=false;
static bool show_bufferusage=true;
static char *meterbridge_type="vu";
static char *meterbridge_reference="0";
//static const int vu_len=56;
#define vu_len 65
static int vu_dB=true;
static float vu_bias=1.0f;
static int leading_zeros=1;
static char *filename_prefix="jack_capture_";
static DEFINE_ATOMIC(int64_t, num_frames_recorded)=0;
static int64_t num_frames_to_record=0;
static bool fixed_duration=false;
static bool no_stdin=false;
static double recording_time=0.0;
static const int disk_error_stop=0;
static int disk_errors=0;
static bool soundfile_format_is_set=false;
static char *soundfile_format="wav";
static char *soundfile_format_one_or_two="wav";
#define ONE_OR_TWO_CHANNELS_FORMAT SF_FORMAT_WAV
static char *soundfile_format_multi="wavex";
#define MORE_THAN_TWO_CHANNELS_FORMAT SF_FORMAT_WAVEX
bool silent=false;
bool verbose=false;
static bool absolutely_silent=false;
static bool create_tme_file=false;
static bool write_to_stdout=false;
static float ogg_vbr_quality = -1.0;
static bool use_jack_transport = false;
static bool use_jack_freewheel = false;
static bool use_manual_connections = false;
#if HAVE_LIBLO
static int osc_port = -1;
#endif
static char *hook_cmd_opened = NULL;
static char *hook_cmd_closed = NULL;
static char *hook_cmd_rotate = NULL;
static char *hook_cmd_timing = NULL;
static int64_t rotateframe=0;

static bool timemachine_mode = false;
static bool timemachine_recording = false;
static float timemachine_prebuffer = 8.0f;
static int64_t num_frames_written_to_disk = 0;
static bool program_ended_with_return = false;

/* JACK data */
static DEFINE_ATOMIC(jack_port_t**, g_ports) = NULL;
static jack_port_t **ports_meterbridge=NULL;
typedef jack_default_audio_sample_t sample_t;
static float jack_samplerate;
static bool jack_has_been_shut_down=false;

static int64_t unreported_overruns=0;
static int total_overruns=0;
static int total_xruns=0;

static volatile int freewheel_mode=0;
static char *jackname="jack_capture"; // limited to 33 characters

static bool disk_thread_has_high_priority=false;

/* Helper thread */
static pthread_t helper_thread={0};
static float *vu_vals=NULL;
static int   *vu_times=NULL;
static int *vu_peaks=NULL;
static float *vu_peakvals=NULL;

/* Synchronization between jack process thread and disk thread. */
static DEFINE_ATOMIC(bool, is_initialized) = false; // This $@#$@#$ variable is needed because jack ports must be initialized _after_ (???) the client is activated. (stupid jack)

// define atomic will just set the variable
static bool is_running_atomic = true;

/* Buffer */
static int block_size; // Set once only. Never changes value after that, even if jack buffer size changes.

static bool buffer_interleaved = true;

typedef struct buffer_t {
  int overruns;
  int pos;
  sample_t data[];
} buffer_t;

//static pid_t mainpid;

static buffer_t *current_buffer;

/* Jack connecting thread. */
static pthread_t connect_thread={0} ;

// das stop semaphore
static SEM_TYPE_T stop_sem;

/////////////////////////////////////////////////////////////////////
//////////////////////// VARIOUS ////////////////////////////////////
/////////////////////////////////////////////////////////////////////

#if 0
void get_free_mem(void){
  struct sysinfo sys_info;

  if(sysinfo(&sys_info) != 0)
    perror("sysinfo");

  printf("Total Ram ----- %uk\tFree Ram ------ %uk\n", sys_info.totalram
         ,
         sys_info.freeram / 1024);
}
#endif
char *string_concat(char *s1,char *s2);

static void verbose_print(const char *fmt, ...) {
  if (absolutely_silent==true) return;

  if(verbose==true) {
    va_list argp;
    va_start(argp,fmt);
    vfprintf(stderr,fmt,argp);
    va_end(argp);
  }
}

static void * my_calloc(size_t size1,size_t size2){
  size_t size = size1 * size2;
  void * ret = malloc(size);

  if(ret == NULL) {
    fprintf(stderr,"\nOut of memory. Try a smaller buffer.\n");
    return NULL;
  }

  // memset replaces the bytes at destination
  // here setting them to 0 in decimal which is the null char
  // is basically cleaning the memory returned by malloc
  memset(ret, 0, size);
  return ret;
}

static bool set_high_priority(void){
  static bool shown_warning = false;
  static int  prio          = -20;

  while(prio<0 && setpriority(PRIO_PROCESS,0,prio)==-1)
    prio++;

  if(prio==0 && shown_warning==false){
    print_message("Warning. Could not set higher priority for a SCHED_OTHER process using setpriority().\n");
    shown_warning=true; }

  if(prio < 0)
    return true;
  else
    return false; }


static int echo_turned_on=true;
static struct termios current;
static struct termios initial;

static void turn_off_echo(void){
  tcgetattr( STDIN_FILENO, &initial );
  current = initial;
  current.c_lflag &= ~ECHO; // added
  tcsetattr( STDIN_FILENO, TCSANOW, &current );

  echo_turned_on=false; }

static void turn_on_echo(void){
  if(echo_turned_on==true)
    return;
  tcsetattr( STDIN_FILENO, TCSANOW, &initial );
  echo_turned_on=true; }


/////////////////////////////////////////////////////////////////////
//////////////////////// BUFFERS ////////////////////////////////////
/////////////////////////////////////////////////////////////////////


static vringbuffer_t *vringbuffer;

//static sample_t **buffers=NULL;
static sample_t *empty_buffer;

static int buffer_size_in_bytes;

//  block size in bytes = jack block size (1 channel) * sizeof(float)
// buffer size in bytes = block size in bytes * num_channels


static int64_t seconds_to_frames(float seconds){
  return (int64_t) (((long double)seconds)*((long double)jack_samplerate));
}


static float frames_to_seconds(int frames){
  return ((float)frames)/jack_samplerate;
}

// round up.
static int seconds_to_blocks(float seconds){
  // jack sample rate is by default 48000
  // and block size by default is 2048
  //
  // so sample rate is the number of samples per seconds
  // and if we have 4 seconds of buffer time
  // that will be 192000 samples divided by 2048 (size of the blocks)
  // results in 93.75 blocks rounded to 94
  return (int)ceilf(
    (seconds * jack_samplerate / (float)block_size));
}

// same return value as seconds_to_blocks
// tranform buffers given in seconds to blocks
static int seconds_to_buffers(float seconds) {
  // by the explanation above we have 94 blocks of buffer size 2048
  // to capture 4 seconds of audio
  return seconds_to_blocks(seconds);
}

#if 0
// not used
static int block_time_in_microseconds(void){
  return (int)(((double)block_size*1000000.0)/(double)jack_samplerate);
}
#endif

static float blocks_to_seconds(int blocks){
  return (float)blocks*(float)block_size/jack_samplerate;
}

// same return value as blocks_to_seconds
static float buffers_to_seconds(int buffers){
  return blocks_to_seconds(buffers);
}


static int autoincrease_callback(
  vringbuffer_t *vrb,
  bool first_call,
  int reading_size,
  int writing_size
) {
  (void)vrb;
  (void)reading_size;

  if(use_jack_freewheel)
    return 0;

  if(first_call){
    set_high_priority();
    return 0; }

  if(timemachine_mode==true && timemachine_recording==false)
    return 0;

#if 0
  if(1){
    static jack_transport_state_t prev_state=-1;
    jack_position_t pos;
    jack_transport_state_t state=jack_transport_query(client,&pos);
    if(state!=prev_state){
      print_message("frame: %d\n",pos.frame);
      prev_state=state; } }
#endif

  if(buffers_to_seconds(writing_size) < min_buffer_time) {
    // autoincrease_callback is called approx
    // at every block, so it should not be necessary
    // to return a value higher than 2.
    // Returning a very low number might also theoretically
    // put a lower constant strain on the memory bus,
    // thus theoretically lower the chance of xruns.
    return 2; 
  }

  return 0;
}

static void buffers_init(){
  verbose_print(
    "bufinit1. sizeof(long): %u, "
    "sizeof(float): %u, "
    "sizeof(double):%u, "
    "sizeof(int):%u, "
    "sizeof(void*):%u\n ",
    sizeof(long),
    sizeof(float),
    sizeof(double),
    sizeof(int),
    sizeof(void*)
  );

  // the seconds_to_buffers function will return
  // the number of blocks of memory needed to capture
  // the buffer size. because both calls are maxed with the number 4,
  // then the first will be 94 (called with -1)
  // and the second 938 (called with 40)
  //
  // buffer_size_in_bytes is set in portnames_add_defaults to 16392
  vringbuffer = vringbuffer_create(
    JC_MAX(4, seconds_to_buffers(min_buffer_time)),
    JC_MAX(4, seconds_to_buffers(max_buffer_time)),
    buffer_size_in_bytes
  );

  if (vringbuffer == NULL) {
    fprintf(stderr,"Unable to allocate memory for buffers\n");
    exit(-1);
  }

  vringbuffer_set_autoincrease_callback(
    vringbuffer, autoincrease_callback, 0);

  current_buffer = vringbuffer_get_writing(vringbuffer);
  empty_buffer   = my_calloc(sizeof(sample_t),block_size*num_channels);
}



/////////////////////////////////////////////////////////////////////
//////////////////////// PORTNAMES //////////////////////////////////
/////////////////////////////////////////////////////////////////////

// this will hold pointers to strings
// with the name of the ports for recording
// exampel system:capture_1
static const char **cportnames = NULL;
static int num_cportnames = 0;

static int findnumports(const char **ports){
  int ret=0;

  while(ports && ports[ret]!=NULL)
    ret++;
  return ret;
}


static void portnames_add_defaults(void){
  if(cportnames==NULL){
    {
      const char **portnames = jack_get_ports(client, NULL, NULL, JackPortIsPhysical|JackPortIsInput);
      int num_ports    = findnumports(portnames);

      if(num_ports==0){
        fprintf(stderr,"No physical output ports found in your jack setup. Exiting.\n");
        exit(0);
      }

      cportnames = my_calloc(sizeof(char*), num_ports+1);
      {
        int i;
        for(i=0;i<num_ports;i++)
          cportnames[i] = portnames[i];
      }

      jack_free(portnames);
    }

    num_cportnames=JC_MAX(DEFAULT_NUM_CHANNELS,findnumports(cportnames));
    if(num_channels==-1) {
      if(!strcasecmp("sds",soundfile_format)){
        num_channels=DEFAULT_NUM_CHANNELS_SDS;
      }else{
        num_channels=DEFAULT_NUM_CHANNELS;
      }
    }

  }else
    if(num_channels==-1)
      num_channels=num_cportnames;

  if(num_channels<=0){
    fprintf(stderr,"No point recording 0 channels. Exiting.\n");
    exit(0);
  }

  // At this point, the variable "num_channels" has a known and valid value.
  vu_vals     = my_calloc(sizeof(float),num_channels);
  vu_times    = my_calloc(sizeof(int),num_channels);
  vu_peaks    = my_calloc(sizeof(int),num_channels);
  vu_peakvals = my_calloc(sizeof(float),num_channels);

  buffer_size_in_bytes = ALIGN_UP_DOUBLE(
      sizeof(buffer_t) + block_size * num_channels * sizeof(sample_t));
  verbose_print("buf_size_in_bytes: %d\n",buffer_size_in_bytes);
}


static void portnames_add(char *name) {
  const char **new_outportnames;
  int add_ch;

  if (name[strlen(name) - 1] == '*') {
    // if an asterisk is given the program will try to
    // find ports on its own.
    char *pattern=strdup(name);
    pattern[strlen(name)-1]=0;

    new_outportnames          = jack_get_ports(client, pattern, "", 0);
    add_ch                    = findnumports(new_outportnames);
    free(pattern);
  } else {
    // sizeof(char*) is the size of the pointer
    // raspberry pi has 4 bytes addresses
    // multiplied by 1 is just allocating 4 bytes of memory
    new_outportnames          = my_calloc(1, sizeof(char*));

    // new_outportnames is a double pointer, which is 4 bytes now
    // setting the first element to be the pointer of the name given
    new_outportnames[0]       = name;
    add_ch                    = 1;
  }

  if (add_ch > 0) {
    int ch;

    // num_cportnames is a static int set to 0 at the top
    // cportnames is a double pointer initialized as null at the top
    // it should hold pointers to the names of ports like system:capture_1

    cportnames = realloc(cportnames, (num_cportnames+add_ch) * sizeof(char*));

    for (ch = 0; ch < add_ch; ch++) {
      cportnames[num_cportnames] = new_outportnames[ch];
      num_cportnames++;
    }

  } else {
    fprintf(stderr,"\nWarning, no port(s) with name \"%s\".\n",name);

    if (cportnames == NULL) {
      if (silent == false) {
	      fprintf(stderr,"This could lead to using default ports instead.\n");
      }
    }
  }
}

static const char **portnames_get_connections(int ch, bool *using_calloc){
  *using_calloc = true; // silence compiler warning. (fix: logic of program is too complicated)

  if(ch>=num_cportnames)
    return NULL;
  else{
    jack_port_t  *port = jack_port_by_name(client, cportnames[ch]);
    const char  **ret;

    if(port==NULL){
      print_message("Error, port with name \"%s\" not found.\n", cportnames[ch]);
      return NULL;
    }

    if(jack_port_flags(port) & JackPortIsInput){
      ret    = jack_port_get_all_connections(client,port);
      *using_calloc = false;
    }else{
      ret    = my_calloc(2,sizeof(char*));
      ret[0] = cportnames[ch];
      *using_calloc = true;
    }

    return ret;
  }
}





/////////////////////////////////////////////////////////////////////
//////////////////////// console meter //////////////////////////////
/////////////////////////////////////////////////////////////////////

// Note that the name "vu" is used instead of "console meter".
// I know (now) it's not a vu at all. :-)


// Function iec_scale picked from meterbridge by Steve Harris.
static int iec_scale(float db) {
         float def = 0.0f; /* Meter deflection %age */

         if (db < -70.0f) {
                 def = 0.0f;
         } else if (db < -60.0f) {
                 def = (db + 70.0f) * 0.25f;
         } else if (db < -50.0f) {
                 def = (db + 60.0f) * 0.5f + 5.0f;
         } else if (db < -40.0f) {
                 def = (db + 50.0f) * 0.75f + 7.5;
         } else if (db < -30.0f) {
                 def = (db + 40.0f) * 1.5f + 15.0f;
         } else if (db < -20.0f) {
                 def = (db + 30.0f) * 2.0f + 30.0f;
         } else if (db < 0.0f) {
                 def = (db + 20.0f) * 2.5f + 50.0f;
         } else {
                 def = 100.0f;
         }

         return (int)(def * 2.0f);
}

static void msleep(int n){
  usleep(n*1000);
}

static void print_ln(void){
  putchar('\n');
  //msleep(3);
}

static void print_console_top(void){
  if(use_vu) {
    int lokke=0;
    char c='"';
    // Set cyan color
    printf("%c[36m",0x1b);

    printf("   |");

    for(lokke=0;lokke<vu_len;lokke++)
      putchar(c);

    printf("|");print_ln();
    printf("%c[0m",0x1b); // reset colors
    fflush(stdout);
  } else {
    //print_ln();
  }
}

static void init_vu(void){
  //int num_channels=4;
  int ch;
  for(ch=0;ch<num_channels;ch++)
    print_ln();
}

static void init_show_bufferusage(void){
  print_ln();
}


static void move_cursor_to_top(void){
  printf("%c[%dA",0x1b,
         use_vu&&show_bufferusage
         ? num_channels+1
         : use_vu
           ? num_channels
           : show_bufferusage
             ? 1
             : 0);
  printf("%c[0m",0x1b); // reset colors
  fflush(stdout);
}

static char *vu_not_recording="-----------Press <Return> to start recording------------";

// Console colors:
// http://www.linuxjournal.com/article/8603

static void print_console(bool move_cursor_to_top_doit, bool force_update){
  //int num_channels=4;
  int ch;
  char vol[vu_len+50];
  vol[2]          = ':';
  vol[3]          = '|';
  vol[4+vu_len+1] = 0;

  // Values have not been updated since last time. Return.
  if(force_update==false && vu_vals[0]==-1.0f)
    return;

  if(move_cursor_to_top_doit)
    move_cursor_to_top();

  if(use_vu){

    // Set cyan color
    printf("%c[36m",0x1b);

    for(ch=0;ch<num_channels;ch++){
      int   i;
      float val   = vu_vals[ch];
      int   pos;
      vu_vals[ch] = -1.0f;

      if(vu_dB)
        pos = iec_scale(20.0f * log10f(val * vu_bias)) * (vu_len) / 200;
      else
        pos = val*(vu_len);

      if (pos > vu_peaks[ch]) {
        vu_peaks[ch]    = pos;
        vu_peakvals[ch] = val;
        vu_times[ch]    = 0;
      } else if (vu_times[ch]++ > 40) {
        vu_peaks[ch]    = pos;
        vu_peakvals[ch] = val;
      }

      if(ch>9){
        vol[0] = '0'+ch/10;
        vol[1] = '0'+ch-(10*(ch/10));
      }else{
        vol[0] = '0';
        vol[1] = '0'+ch;
      }

      if (timemachine_mode==true && timemachine_recording==false) {

        for(i=0;i<pos && val>0.0f;i++)
          vol[4+i] = vu_not_recording[i];

        vol[4+pos]='\0';

        if(vu_peakvals[ch]>=1.0f)
          printf("%c[31m",0x1b); // Peaking, show red color

        printf("%s", vol);

        for(;i<vu_len;i++)
          vol[4+i] = vu_not_recording[i];

        printf("%c[33m",0x1b); // Yellow color

        vol[i+4]='\0';
        printf("%s", vol+4+pos);

        printf("%c[36m",0x1b); // back to cyan
        printf("|\n");

      } else {

        for(i=0;i<vu_len;i++)
          if(vu_peaks[ch]==i && vu_peakvals[ch]>0.0f)
            vol[4+i] = '*';
          else if(i<=pos && val>0.0f)
            vol[4+i] = '-';
          else
            vol[4+i] = ' ';

        if(vu_peakvals[ch]>=1.0f){
          vol[4+vu_len]='!';
          printf("%c[31m",0x1b); //red color
          puts(vol);
          printf("%c[36m",0x1b); // back to cyan
        }else{
          vol[4+vu_len]='|';
          puts(vol);
        }

      }
    }
  }

  if(show_bufferusage){
    int   num_bufleft = vringbuffer_writing_size(vringbuffer);
    int   num_buffers = (vringbuffer_reading_size(vringbuffer)+ vringbuffer_writing_size(vringbuffer));
    float buflen      = buffers_to_seconds(num_buffers);
    float bufleft     = buffers_to_seconds(num_bufleft);
    int   recorded_seconds = (int)frames_to_seconds(ATOMIC_GET(num_frames_recorded));
    if(timemachine_mode==true)
      recorded_seconds = (int)frames_to_seconds(num_frames_written_to_disk);
    int   recorded_minutes = recorded_seconds/60;

    char buffer_string[1000];
    {
      sprintf(buffer_string,"%.2fs./%.2fs",bufleft,buflen);
      int len_buffer=strlen(buffer_string);
      int i;
      for(i=len_buffer;i<14;i++)
        buffer_string[i]=' ';
      buffer_string[i]='\0';
    }

    printf("%c[32m"
           "Buffer: %s"
           "  Time: %d.%s%dm.  %s"
           "DHP: [%c]  "
           "Overruns: %d  "
           "Xruns: %d"
           "%c[0m",
           //fmaxf(0.0f,buflen-bufleft),buflen,
           0x1b, // green color
           buffer_string,
           recorded_minutes, recorded_seconds%60<10?"0":"", recorded_seconds%60, recorded_minutes<10?" ":"",
           disk_thread_has_high_priority?'x':' ',
           total_overruns,
           total_xruns,
           0x1b // reset color
           );
    print_ln();
  }else{
    printf("%c[0m",0x1b); // reset colors
    fprintf(stderr,"%c[0m",0x1b); // reset colors
  }
  fflush(stdout);
  fflush(stderr);
}



/////////////////////////////////////////////////////////////////////
//////////////////////// Helper thread //////////////////////////////
/////////////////////////////////////////////////////////////////////

#define MESSAGE_PREFIX ">>> "

static char message_string[5000]={0};

static volatile int helper_thread_running=0;
static int init_meterbridge_ports();

static bool is_helper_running=true;

static void *helper_thread_func(void *arg){
  (void)arg;

  helper_thread_running=1;

  if(use_vu||show_bufferusage)
    print_console_top();

  if(use_vu)
    init_vu();

  if(show_bufferusage)
    init_show_bufferusage();

  do {
    bool move_cursor_to_top_doit=true;

    if(message_string[0] != 0) {
      if(use_vu || show_bufferusage){
	move_cursor_to_top();
        if(!use_vu){
          print_ln();
        }
	printf("%c[%dA",0x1b,1); // move up yet another line.
        msleep(5);
	printf("%c[31m",0x1b);   // set red color
	{ // clear line
	  int lokke;
	  for(lokke=0;lokke<vu_len+5;lokke++)
	    putchar(' ');
          print_ln();
	  printf("%c[%dA",0x1b,1); // move up again
          msleep(5);
	}
      }
      printf(MESSAGE_PREFIX);
      printf("%s",message_string);
      message_string[0]=0;
      move_cursor_to_top_doit=false;
      if(use_vu || show_bufferusage)
	print_console_top();
    }

    if(use_vu || show_bufferusage)
      print_console(move_cursor_to_top_doit,false);

    if(init_meterbridge_ports()==1 && use_vu==false && show_bufferusage==false) // Note, init_meterbridge_ports will usually exit at the top of the function, where it tests for ports_meterbridge!=NULL (this stuff needs to be handled better)
      break;

    msleep(1000/20);

  }while(is_helper_running);


  if(use_vu || show_bufferusage){
    print_console(true,true);
  }

  message_string[0]     = 0;
  helper_thread_running = 0;

  msleep(4);

  return NULL;
}

enum ThreadType{
  UNINITIALIZED_THREAD_TYPE,
  OTHER_THREAD,
  REALTIME_THREAD,
  MAIN_THREAD
};

static __thread enum ThreadType g_thread_type = UNINITIALIZED_THREAD_TYPE;

static pthread_mutex_t print_message_mutex = PTHREAD_MUTEX_INITIALIZER;
void print_message(const char *fmt, ...){
  if (absolutely_silent==true) return;

  if (g_thread_type == REALTIME_THREAD)
    // what?
    return;

  if(helper_thread_running==0 || write_to_stdout==true){

    va_list argp;
    va_start(argp,fmt);
    fprintf(stderr,"%c[31m" MESSAGE_PREFIX,0x1b);   // set red color
    vfprintf(stderr,fmt,argp);
    fprintf(stderr,"%c[0m",0x1b); // reset colors
    fflush(stderr);
    va_end(argp);

  }else{

    pthread_mutex_lock(&print_message_mutex);{

      while(message_string[0]!=0)
        msleep(2);

      va_list argp;
      va_start(argp,fmt);
      vsprintf(message_string,fmt,argp);
      va_end(argp);

      while(message_string[0]!=0)
        msleep(2);

    }pthread_mutex_unlock(&print_message_mutex);
  }

}


void setup_helper_thread (void){
  if(write_to_stdout==false){
    pthread_create(&helper_thread, NULL, helper_thread_func, NULL);
  }
}

static void stop_helper_thread(void){
  //helper_thread_running=0;
  is_helper_running=false;
  if(write_to_stdout==false){
    pthread_join(helper_thread, NULL);
  }

  /*
  if(use_vu||show_bufferusage){
    printf("%c[0m",0x1b); // reset colors
    usleep(1000000/2); // wait for terminal
  }
  */
}

/////////////////////////////////////////////////////////////////////
//////////////////////// DISK Thread hooks //////////////////////////
/////////////////////////////////////////////////////////////////////



#define ARGS_ADD_ARGV(FMT,ARG) \
  argv=(char**) realloc((void*)argv, (argc+2)*sizeof(char*)); \
  asprintf(&argv[argc++], FMT, ARG); argv[argc] = 0;

#define PREPARE_ARGV(CMD,ARGC,ARGV) \
{\
  char *bntmp = strdup(CMD); \
  ARGC=0; \
  ARGV=(char**) calloc(2,sizeof(char*)); \
  ARGV[ARGC++] = strdup(basename(bntmp));\
  free(bntmp); \
}

static void wait_child(int sig){
  (void)sig;
  wait(NULL);
}

static void call_hook(const char *cmd, int argc, char **argv){
  /* invoke external command */
  if (verbose==true) {
    fprintf(stderr, "EXE: %s ", cmd);
    for (argc=0;argv[argc];++argc) printf("'%s' ", argv[argc]);
    printf("\n");
  }

  pid_t pid=fork();

  if (pid==0) {
    /* child process */

    if(1){ /* redirect all output of child process*/
      /* one day this if(1) could become a global option */
      int fd;
      if((fd = open("/dev/null", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR))==-1){
	perror("open");
      }else{
	dup2(fd,STDOUT_FILENO);
	dup2(fd,STDERR_FILENO);
	close(fd);
      }
    }
    execvp (cmd, (char *const *) argv);
    print_message("EXE: error; exec returned.\n");
    //pause();
    exit(127);
  }

  /* parent/main process */
  if (pid < 0 ) {
    print_message("EXE: error; can not fork child process\n");
  }

  signal(SIGCHLD,wait_child);

  /* clean up */
  for (argc=0;argv[argc];++argc) {
    free(argv[argc]);
  }
  free (argv);
}

static void hook_file_opened(char *fn){
  char **argv; int argc;
  const char *cmd = hook_cmd_opened;
  if (!cmd) return;
  PREPARE_ARGV(cmd, argc, argv);
  ARGS_ADD_ARGV("%s", fn);
  call_hook(cmd, argc, argv);
}

static void hook_rec_timimg(char *fn, struct timespec start, jack_nframes_t latency){
  char **argv; int argc;
  const char *cmd = hook_cmd_timing;
  if (!cmd) return;
  PREPARE_ARGV(cmd, argc, argv);
  ARGS_ADD_ARGV("%s", fn);
  ARGS_ADD_ARGV("%ld", start.tv_sec);
  ARGS_ADD_ARGV("%ld", start.tv_nsec);
  ARGS_ADD_ARGV("%d", latency);
  call_hook(cmd, argc, argv);
}

static void hook_file_closed(char *fn, int xruns, int io_errors){
  char **argv; int argc;
  const char *cmd = hook_cmd_closed;
  if (!cmd) return;
  PREPARE_ARGV(cmd, argc, argv);
  ARGS_ADD_ARGV("%s", fn);
  ARGS_ADD_ARGV("%d", xruns);
  ARGS_ADD_ARGV("%d", io_errors);
  call_hook(cmd, argc, argv);
}

static void hook_file_rotated(char *oldfn, char *newfn, int num, int xruns, int io_errors){
  char **argv; int argc;
  const char *cmd = hook_cmd_rotate;
  if (!cmd) return;
  PREPARE_ARGV(cmd, argc, argv);
  ARGS_ADD_ARGV("%s", oldfn);
  ARGS_ADD_ARGV("%d", xruns);
  ARGS_ADD_ARGV("%d", io_errors);
  ARGS_ADD_ARGV("%s", newfn);
  ARGS_ADD_ARGV("%d", num);
  call_hook(cmd, argc, argv);
}


/////////////////////////////////////////////////////////////////////
//////////////////////// DISK ///////////////////////////////////////
/////////////////////////////////////////////////////////////////////

// These four variables are used in case we break the 4GB barriere for standard wav files.
static int num_files=1;
static int64_t disksize=0;
static bool is_using_wav=true;
static int bytes_per_frame;

static SNDFILE *soundfile=NULL;
static int64_t overruns=0;

#if HAVE_LIBLO
bool queued_file_rotate=false;
 void osc_stop() { SEM_SIGNAL(stop_sem); }
void osc_tm_start() { timemachine_recording=true; }
void osc_tm_stop() { program_ended_with_return=true; osc_stop(); }
#endif

static struct timespec rtime;
static DEFINE_ATOMIC(int, g_store_sync) = 0;
static int ssync_offset = 0;
static jack_nframes_t j_latency = 0;

#include "setformat.c"


static int open_soundfile(void){
  int subformat;

  SF_INFO sf_info; memset(&sf_info,0,sizeof(sf_info));

  if(write_to_stdout==true) {
	hook_file_opened("file:///stdout");
    return 1;
	}


  if(filename==NULL){
    if (rotateframe>0){ // always use .NUM. with rotation
      filename=my_calloc(1,strlen(base_filename)+500);
      sprintf(filename,"%s.%0*d.%s",base_filename,leading_zeros+1,0,soundfile_format);
    }else{
      filename=strdup(base_filename);
    }
  }

  /////////////////////////
  // Code below for sndfile
  /////////////////////////

  sf_info.samplerate = jack_samplerate;
  sf_info.channels   = num_channels;

  {
    int format=getformat(soundfile_format);
    if(format==-1 && num_channels>2){
      fprintf(stderr,"Warning, the format \"%s\" is not supported. Using \"%s\" instead.\n", soundfile_format, soundfile_format_multi);
      sf_info.format=MORE_THAN_TWO_CHANNELS_FORMAT;
    }else if(format==-1){
      fprintf(stderr,"Warning, the format \"%s\" is not supported. Using \"%s\" instead.\n", soundfile_format, soundfile_format_one_or_two);
      sf_info.format=ONE_OR_TWO_CHANNELS_FORMAT;
    }else
      sf_info.format=format;
  }

  is_using_wav = ((sf_info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV)?true:false;

  switch (bitdepth) {
  case 8: subformat = SF_FORMAT_PCM_U8;
    break;
  case 16: subformat = SF_FORMAT_PCM_16;
    break;
  case 24: subformat = SF_FORMAT_PCM_24;
    break;
  case 32: subformat = SF_FORMAT_PCM_32;
    break;
  default:
    if(!strcasecmp("flac",soundfile_format) || !strcasecmp("sds",soundfile_format)){
      bitdepth=24;
      subformat=SF_FORMAT_PCM_24;
    }else if((sf_info.format & SF_FORMAT_SUBMASK)) {
      // getformat has set subformat (SF_FORMAT_VORBIS or SF_FORMAT_OPUS).
      subformat = sf_info.format & SF_FORMAT_SUBMASK;
    }else{
      bitdepth=32; // sizeof(float)*8 would be incorrect in case sizeof(float)!=4
      subformat = SF_FORMAT_FLOAT;
    }
    break;
  }

  bytes_per_frame=bitdepth/8;

  sf_info.format |= subformat;

  if(sf_format_check(&sf_info)==0){
    fprintf (stderr, "\nFileformat not supported by libsndfile. Try other options.\n");
    return 0;
  }

  if(write_to_stdout==true)
    soundfile=sf_open_fd(fileno(stdout),SFM_WRITE,&sf_info,false); // ??? this code is never reached.
  else
    soundfile=sf_open(filename,SFM_WRITE,&sf_info);

  // debugging lines below.
  //static int ai=0;
  //ai++;
  if(soundfile==NULL){ // || ai==10){
    fprintf (stderr, "\nCan not open sndfile \"%s\" for output (%s)\n", filename,sf_strerror(NULL));
    return 0;
  }

#if HAVE_OGG
  if (subformat == SF_FORMAT_VORBIS && ogg_vbr_quality>=-0.1 && ogg_vbr_quality<=1.0){ // assuming quality range from libvorbis
	  double vbr_quality = (double) ogg_vbr_quality; // convert from float to double (libvorbis uses float BTW)
	  sf_command(soundfile, SFC_SET_VBR_ENCODING_QUALITY, &vbr_quality, sizeof(double));
  }
#endif

  hook_file_opened(filename);

  return 1;
}


static void close_soundfile(void){

  if(write_to_stdout==false){
    if(soundfile!=NULL)
      sf_close (soundfile);
  }

  hook_file_closed(filename, total_overruns + total_xruns, disk_errors);

  if (overruns > 0) {
    print_message("jack_capture failed with a total of %d overruns.\n", total_overruns);
    print_message("   try a bigger buffer than -B %f\n",min_buffer_time);
  }
  if (disk_errors > 0)
    print_message("jack_capture failed with a total of %d disk errors.\n",disk_errors);
  if (total_xruns > 0)
    print_message("jack_capture encountered %d jack x-runs.\n", total_xruns);

  disk_errors = 0;
  total_overruns = 0;
  total_xruns = 0;
}

static int rotate_file(size_t frames, int reset_totals){
  ATOMIC_SET(g_store_sync, 0);
	// Explanation: new file will already contain the CURRENT buffer!
	// but sync-timeframe will only be saved on the start of next jack cycle.
	//
	// -> save current audio ringbuffer-size -> subtract from next sync timestamp.
	ssync_offset = frames;

  sf_close(soundfile);

  char *filename_new;
  filename_new=my_calloc(1,strlen(base_filename)+500);
  sprintf(filename_new,"%s.%0*d.%s",base_filename,leading_zeros+1,num_files,soundfile_format);
  print_message("Closing %s, and continue writing to %s.\n",filename,filename_new);
  num_files++;

  hook_file_rotated(filename, filename_new, num_files, total_overruns + total_xruns, disk_errors);

  free(filename);
  filename=filename_new;
  disksize=0;

  if (reset_totals) {
    if (overruns > 0) {
      print_message("jack_capture failed with a total of %d overruns.\n", total_overruns);
      print_message("   try a bigger buffer than -B %f\n",min_buffer_time);
    }
    if (disk_errors > 0)
      print_message("jack_capture failed with a total of %d disk errors.\n",disk_errors);
    if (total_xruns > 0)
      print_message("jack_capture encountered %d jack x-runs.\n", total_xruns);

    /* reset totals on file-rotate */
    disk_errors = 0;
    total_overruns = 0;
    total_xruns = 0;
  }

  if(!open_soundfile()) return 0;

  return 1;
}

// To test filelimit handler, uncomment two next lines.
//#undef UINT32_MAX
//#define UINT32_MAX 100000+(1024*1024)

static int handle_filelimit(size_t frames){
  int new_bytes=frames*bytes_per_frame*num_channels;

  if(is_using_wav && (disksize + ((int64_t)new_bytes) >= UINT32_MAX-(1024*1024))){ // (1024*1024) should be enough for the header.
    print_message("Warning. 4GB limit on wav file almost reached.");
    if (!rotate_file(frames, false)) return 0;
  }
#if HAVE_LIBLO
  else if (queued_file_rotate) {
    queued_file_rotate=false;
    print_message("Note. file-name rotation request received.");
    if (!rotate_file(frames, true)) return 0;
  }
#endif
  else if (rotateframe > 0 && disksize > (rotateframe*bytes_per_frame*num_channels) ) {
    if (!rotate_file(frames, false)) return 0;
	}
  disksize+=new_bytes;
  return 1; }



// stdout_write made by looking at http://mir.dnsalias.com/oss/jackstdout/start
// made by Robin Gareus.
static int stdout_write(sample_t *buffer,size_t frames){
  static char *tobuffer=NULL;
  static int bufferlen=0;
  int bytes_to_write=frames*num_channels*2;

  if(bufferlen<bytes_to_write){
    free(tobuffer);
    tobuffer=my_calloc(1,bytes_to_write);
    bufferlen=bytes_to_write; }

  {
    unsigned int i;
    int writeplace=0;
    for(i=0;i<frames*num_channels;i++){
      int d = (int) rint(buffer[i]*32767.0);
      tobuffer[writeplace++] = (unsigned char) (d&0xff);
      tobuffer[writeplace++] = (unsigned char) (((d&0xff00)>>8)&0xff); } }

  {
    int   fd           = fileno(stdout);
    char *tobuffer_use = tobuffer;

    while(bytes_to_write > 0){
      int written=write(fd,tobuffer_use,bytes_to_write);
      if(written==-1){
	fprintf(stderr,"Error writing to stdout.\n");
	break;
      }
      bytes_to_write -= written;
      tobuffer_use   += written; } }

  return 1; }

static int disk_write(void *data,size_t frames){

  num_frames_written_to_disk += frames;

  if(write_to_stdout==true)
    return stdout_write(data,frames);

  if(soundfile==NULL)
    return 0;

  if(!handle_filelimit(frames))
    return 0;

  if((size_t)sf_writef_float(soundfile,data,frames) != frames){
    print_message("Error. Can not write sndfile (%s)\n",
		sf_strerror(soundfile)
		);
    disk_errors++;
    return 0;
  }
  return 1;
}


static int disk_write_overruns(int num_overruns){
  if(verbose==true)
    print_message(
                  "jack_capture failed writing %d frame%s. Some parts of the recording will contain silence.\n"
                  "    Try a bigger buffer than -B %f\n%s",
                  num_overruns,num_overruns==1 ? "" : "s",
                  min_buffer_time,
                  __atomic_load_n(&is_running_atomic, __ATOMIC_SEQ_CST) ? "Continue recording...\n" : ""
                  );

  overruns+=num_overruns;

  while(num_overruns>0){
    int size=JC_MIN(block_size,num_overruns);
    if( ! disk_write(empty_buffer,size))
      return 0;
    num_overruns-=size;
  }

  return 1;
}

static void disk_thread_control_priority(void){
  int adjusted_writing_size = vringbuffer_writing_size(vringbuffer);
  if (timemachine_mode==true)
    adjusted_writing_size += seconds_to_blocks(timemachine_prebuffer*jack_samplerate);

  if(1
     && disk_thread_has_high_priority==false
     && vringbuffer_reading_size(vringbuffer) >= adjusted_writing_size
     && use_jack_freewheel==false
     )
    {
      if(set_high_priority()==true){
        disk_thread_has_high_priority=true;
        print_message("Less than half the buffer used. Setting higher priority for the disk thread.\n");
      }else{
        static bool message_sent=false;
        if(message_sent==false)
          print_message("Error. Could not set higher priority for disk thread.\n");
        message_sent=true; } } }

static enum vringbuffer_receiver_callback_return_t disk_callback(vringbuffer_t *vrb,bool first_time,void *element){
  static bool printed_receive_message=false;
  buffer_t *buffer=(buffer_t*)element;

  if (first_time==true) {
    return true;
  }

  if (timemachine_mode==true && timemachine_recording==false) {
    int num_buffers = vringbuffer_reading_size(vrb);
    if (buffers_to_seconds(num_buffers) > timemachine_prebuffer)
      return VRB_CALLBACK_USED_BUFFER; // i.e throw away the buffer.
    else
      return VRB_CALLBACK_DIDNT_USE_BUFFER;
  }

  if (timemachine_mode==true && printed_receive_message==false){
    print_message("Recording. Press <Return> to stop.\n");
    printed_receive_message=true; }

  if(use_jack_transport==true && printed_receive_message==false){
    print_message("Received JackTranportRolling. Recording.\n");
    printed_receive_message=true; }

  if(use_jack_freewheel==true && printed_receive_message==false){
    print_message("Entered Jack Freewheeling. Recording.\n");
    printed_receive_message=true; }

  disk_thread_control_priority();


  if (ATOMIC_COMPARE_AND_SET_INT(g_store_sync, 1, 2)) {

    hook_rec_timimg(filename, rtime, j_latency);

    if (create_tme_file) { /* write .tme info-file */
      /*subtract port latency.*/
      int64_t lat_nsec = 1000000000 * j_latency / jack_samplerate;
      int64_t lat_sec = lat_nsec/1000000000;
      lat_nsec = lat_nsec%1000000000;

      if (rtime.tv_nsec >= lat_nsec) rtime.tv_nsec-=lat_nsec; else {rtime.tv_nsec+=(1000000000-lat_nsec); rtime.tv_sec--;}
      rtime.tv_sec-=lat_sec;

      /* subtract (buffer) offset after file-rotate */
      int64_t sync_nsec = (ssync_offset%((int)jack_samplerate))*1000000000/jack_samplerate;
      int64_t sync_sec  = ssync_offset/((int)jack_samplerate);

      if (rtime.tv_nsec > sync_nsec) rtime.tv_nsec-=sync_nsec; else {rtime.tv_nsec+=(1000000000-sync_nsec); rtime.tv_sec--;}
      rtime.tv_sec-=sync_sec;
      ssync_offset = 0;

      /* ok, write to file */
      FILE *file = fopen(string_concat(filename, ".tme"),"w");
      if(file) {
	fprintf(file, "%ld.%ld\n", rtime.tv_sec, rtime.tv_nsec);
	fprintf(file, "# port-latency: %d frames\n", j_latency);
	fprintf(file, "# sample-rate : %f samples/sec\n", jack_samplerate);

	char tme[64]; struct tm time;
	gmtime_r(&rtime.tv_sec, &time);
	strftime(tme, 63, "%F %T", &time);
	fprintf(file, "# system-time : %s.%ld UTC\n", tme, rtime.tv_nsec);

	fclose(file);
      }
    }
  }

  if( buffer->overruns > 0)
    disk_write_overruns(buffer->overruns);

  disk_write(buffer->data,buffer->pos);

  return VRB_CALLBACK_USED_BUFFER;}


static void cleanup_disk(void){

  // Adding silence at the end. Not much point.
  if(unreported_overruns>0)
    disk_write_overruns(unreported_overruns);

  close_soundfile();

  if(verbose==true)
    fprintf(stderr,"disk thread finished\n");
}





/////////////////////////////////////////////////////////////////////
//////////////////////// JACK PROCESS ///////////////////////////////
/////////////////////////////////////////////////////////////////////


static void send_buffer_to_disk_thread(buffer_t *buffer){
  buffer->overruns = unreported_overruns;
  vringbuffer_return_writing(vringbuffer,buffer);
  unreported_overruns = 0;
}


static void process_fill_buffer(sample_t *in[],buffer_t *buffer,int i,int end){
  sample_t *data=buffer->data;
  int pos=buffer->pos*num_channels;
  int ch;

  if(buffer_interleaved == true){
    for(;i<end;i++){
      for(ch=0;ch<num_channels;ch++){
        sample_t val=in[ch][i];
        data[pos++]=val;
        val=fabsf(val);
        if(val>safe_float_read(&vu_vals[ch]))
          safe_float_write(&vu_vals[ch], val);
      }
    }
  }else{
    int start_i = i;
    for(ch=0;ch<num_channels;ch++){
      sample_t *curr_in=in[ch];
      float max_vu=safe_float_read(&vu_vals[ch]);
      for(i = start_i; i<end ; i++){
        sample_t val=curr_in[i];
        data[pos++]=val * 32767.9; // weird lame format (lame is currently the only one using non-interleaved buffers, so the multiplication can be done here)
        val=fabsf(val);
        if(val>max_vu)
          max_vu=val;
      }
      safe_float_write(&vu_vals[ch], max_vu);
    }
  }
  //fprintf(stderr,"pos: %d %d\n",pos,num_channels);
  buffer->pos=pos/num_channels;
}

static bool process_new_current_buffer(int frames_left){
  if (use_jack_freewheel==true) {
    while (vringbuffer_writing_size(vringbuffer)==0)
      msleep(2);
  }

  current_buffer=(buffer_t*)vringbuffer_get_writing(vringbuffer);
  if(current_buffer==NULL){
    total_overruns++;
    unreported_overruns += frames_left;
    return false;
  }
  current_buffer->pos=0;
  return true;
}

static void process_fill_buffers(int jack_block_size){
  sample_t *in[num_channels];
  int i=0,ch;

  jack_port_t **ports = ATOMIC_GET(g_ports);

  for(ch=0;ch<num_channels;ch++)
    in[ch]=jack_port_get_buffer(ports[ch],jack_block_size);

  if(current_buffer==NULL && process_new_current_buffer(jack_block_size)==false)
    return;

  while(i<jack_block_size){
    int size=JC_MIN(jack_block_size - i,
                    block_size - current_buffer->pos
                    );

    process_fill_buffer(in,current_buffer,i,i+size);

    i+=size;

    if(current_buffer->pos == block_size){
      send_buffer_to_disk_thread(current_buffer);
      if(process_new_current_buffer(jack_block_size-i)==false)
        return;
    }

  }
}

static bool jack_transport_started=false;
static bool jack_freewheel_started=false;

enum ProcessState{
  NOT_STARTED,
  HANDLING_LATENCY,
  RECORDING,
  RECORDING_FINISHED
};

static enum ProcessState process_state=NOT_STARTED;

static int process(jack_nframes_t nframes, void *arg){
  (void)arg;

  if (g_thread_type == UNINITIALIZED_THREAD_TYPE)
    g_thread_type = REALTIME_THREAD;

  jack_transport_state_t jack_transport_state=JackTransportStopped;

  // jack_transport
  if(use_jack_transport==true){
    jack_transport_state=jack_transport_query(client,NULL);
    if(jack_transport_state==JackTransportRolling){
      jack_transport_started=true;
    }

    if(jack_transport_started==false)
      return 0;
  }

  // jack_freewheel
  if (use_jack_freewheel==true) {
    if (freewheel_mode > 0)
      jack_freewheel_started=true;

    if(jack_freewheel_started==false)
      return 0;
  }

  if(ATOMIC_GET(is_initialized)==false)
    return 0;

  if(__atomic_load_n(&is_running_atomic, __ATOMIC_SEQ_CST) == false) {
    return 0;
  }

  if(process_state==RECORDING_FINISHED)
    return 0;

  jack_port_t **ports = ATOMIC_GET(g_ports);

  if (ATOMIC_GET(g_store_sync)==0) {
#ifndef NEW_JACK_LATENCY_API
    int ch;
    j_latency=0;
    for(ch=0;ch<num_channels;ch++) {
      const jack_nframes_t jpl= jack_port_get_total_latency(client,ports[ch]);
      if (j_latency < jpl) j_latency = jpl;
    }
#endif
    clock_gettime(CLOCK_REALTIME, &rtime);
    ATOMIC_SET(g_store_sync, 1);
  }

  if(fixed_duration==true){     // User has specified a duration
    int num_frames;

    num_frames=JC_MIN(nframes,num_frames_to_record - ATOMIC_GET(num_frames_recorded));

    if(num_frames>0)
      process_fill_buffers(num_frames);

    ATOMIC_ADD(num_frames_recorded, num_frames);

    if(ATOMIC_GET(num_frames_recorded)==num_frames_to_record){
      send_buffer_to_disk_thread(current_buffer);
      SEM_SIGNAL(stop_sem);
      process_state=RECORDING_FINISHED;
    }

  }else{

    process_fill_buffers(nframes);

    ATOMIC_ADD(num_frames_recorded, nframes);

    if((   use_jack_transport==true && jack_transport_state==JackTransportStopped)
       || (use_jack_freewheel==true && freewheel_mode==0)
       )
      {
        send_buffer_to_disk_thread(current_buffer);
        SEM_SIGNAL(stop_sem);
        process_state=RECORDING_FINISHED;
      }

  }

  vringbuffer_trigger_autoincrease_callback(vringbuffer);

  return 0;
}

static int xrun(void *arg){
  (void)arg;
  total_xruns++;
  return 0;
}


/////////////////////////////////////////////////////////////////////
/////////////////// METERBRIDGE /////////////////////////////////////
/////////////////////////////////////////////////////////////////////


static char* meterbridge_jackname;
pid_t meterbridge_pid;


static void start_meterbridge(int num_channels){

  meterbridge_jackname=my_calloc(1,5000);

  sprintf(meterbridge_jackname,"%s_meterbridge",jack_get_client_name(client));
  //meterbridge -t vu -n meterbri xmms-jack_12250_000:out_0 xmms-jack_12250_000:out_1

  meterbridge_pid=fork();
  if(meterbridge_pid==0){
    char *argv[100+num_channels];
    argv[0] = "meterbridge";
    argv[1] = "-t";
    argv[2] = meterbridge_type;
    argv[3] = "-n";
    argv[4] = meterbridge_jackname;
    argv[5] = "-r";
    argv[6] = meterbridge_reference;
    {
      int ch;
      for(ch=0;ch<num_channels;ch++){
	argv[7+ch]="x";
      }
      argv[7+ch]=NULL;
    }


    if(1){ // Same as adding ">/dev/null 2>/dev/null" in a shell
      int fd;
      if((fd = open("/dev/null", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR))==-1){
	perror("open");
      }else{
	dup2(fd,STDOUT_FILENO);
	dup2(fd,STDERR_FILENO);
	close(fd);
      }
    }

    execvp("meterbridge",argv);

    pause(); // Prevent anyone else using the meterbridge_pid. (parent kills meterbridge_pid at exit, whether it was successfully started or not)

    exit(0);
  }
}


static void wake_up_connection_thread(void);
int connect_meterbridge=0;

static int init_meterbridge_ports(){
  if(ports_meterbridge!=NULL || use_meterbridge==false)
    return 0;

  {
    char portname[5000];
    sprintf(portname,"%s:meter_1",meterbridge_jackname);
    jack_port_t *port1=jack_port_by_name(client,portname);

    if(port1==NULL){
      return 0;
    }

    {
      jack_port_t **ports_meterbridge2 = (jack_port_t **) my_calloc (sizeof (jack_port_t *),num_channels);

      ports_meterbridge2[0]=port1;
      {
	int ch;
	for(ch=1;ch<num_channels;ch++){
	  sprintf(portname,"%s:meter_%d",meterbridge_jackname,ch+1);
	  ports_meterbridge2[ch]=jack_port_by_name(client,portname);
	  if(ports_meterbridge2[ch]==NULL){
	    print_message("Warning! Something is very wrong with the meterbridge.\n"); // Well, maybe that port hasn't been initializied yet. Must fix some
	    free(ports_meterbridge2);
	    return 0;
	  }
	}
      }
      ports_meterbridge=ports_meterbridge2;
      connect_meterbridge=1;
      wake_up_connection_thread();
      //connect_ports(ports_meterbridge);
      return 1;
    }
  }
}




/////////////////////////////////////////////////////////////////////
/////////////////// JACK CONNECTIONS ////////////////////////////////
/////////////////////////////////////////////////////////////////////


static void free_jack_connections(bool using_calloc, const char **connections) {
  if (connections==NULL)
    return;
  if (using_calloc)
    free(connections);
  else
    jack_free(connections);
}

static int compare(const void *a, const void *b){
  return strcmp((const char*)a,(const char*)b);
}

static int reconnect_ports_questionmark(void){
  int ch;

  jack_port_t **ports = ATOMIC_GET(g_ports);

  for(ch=0;ch<num_channels;ch++){
    bool using_calloc;
    const char **connections1 = portnames_get_connections(ch, &using_calloc);
    const char **connections2 = jack_port_get_all_connections(client,ports[ch]);

    int memb1 = findnumports(connections1);
    int memb2 = findnumports(connections2);

    if(memb1==0 && memb2==0)
      continue;

    if(memb1!=memb2){
      free_jack_connections(using_calloc, connections1);
      free_jack_connections(false, connections2);
      return 1;
    }

    qsort(connections1,memb1,sizeof(char*),compare);
    qsort(connections2,memb2,sizeof(char*),compare);

    {
      int lokke = 0;
      for(lokke=0;lokke<memb1;lokke++){
        //printf("connect_ports \"%s\" \"%s\" \n",connections1[lokke],connections2[lokke]);
        if(strcmp(connections1[lokke],connections2[lokke])){
          free_jack_connections(using_calloc, connections1);
          free_jack_connections(false, connections2);
          return 1;
        }
      }
    }

    free_jack_connections(using_calloc, connections1);
    free_jack_connections(false, connections2);
  }

  return 0;
}


static void disconnect_ports(jack_port_t** ports){
  int ch;

  if(ports==NULL)
    return;

  for(ch=0;ch<num_channels;ch++){
    int lokke = 0;
    const char **connections = jack_port_get_all_connections(client,ports[ch]);

    if(connections)
      for(;connections[lokke]!=NULL;lokke++)
        jack_disconnect(client,connections[lokke],jack_port_name(ports[ch]));

    free_jack_connections(false, connections);
  }
}


static void connect_ports(jack_port_t** ports){
  int ch;

  if(ports==NULL)
    return;

  for(ch=0;ch<num_channels;ch++){
    int lokke = 0;

    bool using_calloc;
    const char **connections = portnames_get_connections(ch, &using_calloc);

    while(connections && connections[lokke] != NULL){
      int err=jack_connect(client,connections[lokke],jack_port_name(ports[ch]));
      if(err!=0)
	print_message("\nCould not connect input port %s to %s, errorcode %s\n",
                      jack_port_name (ports[ch]), connections[lokke],strerror(err));
      lokke++;
    }
    free_jack_connections(using_calloc, connections);
  }
}

static sem_t connection_semaphore;

static void * connection_thread(void *arg) {
  (void)arg;

  while(1) {
    // printf("start connection thread while loop\n");
    sem_wait(&connection_semaphore);

    // atomic functions comply with C++11 memory model
    if(__atomic_load_n(&is_running_atomic, __ATOMIC_SEQ_CST) == false) {
      printf("goto done\n");
      goto done;
    }

    if (connect_meterbridge == 1) {
      printf("enter meterbridge\n");
      connect_ports(ports_meterbridge);
      connect_meterbridge = 0;
      continue;
    }

    if (ATOMIC_GET(is_initialized) && reconnect_ports_questionmark()) {
      printf("enter is initialized and reconnect ports is true\n");
      if (silent == false) {
        print_message("Reconnecting ports.\n");
      }

      jack_port_t **ports = ATOMIC_GET(g_ports);

      disconnect_ports(ports);
      connect_ports(ports);

      disconnect_ports(ports_meterbridge);
      connect_ports(ports_meterbridge);
    }
  }

 done:
  if (verbose == true) {
    fprintf(stderr,"connection thread finished\n");
  }
  return NULL;
}

static void wake_up_connection_thread(void) {
  if(use_manual_connections==false)
    sem_post(&connection_semaphore);
}

static void start_connection_thread(void) {
  sem_init(&connection_semaphore, 0, 0);
  pthread_create(&connect_thread, NULL, connection_thread, NULL);
}

static void stop_connection_thread(void) {
  wake_up_connection_thread();
  pthread_join(connect_thread, NULL);
}

static int graphordercallback(void *arg){
  (void)arg;

  if (!freewheel_mode)
    wake_up_connection_thread();
  return 0;
}

static void freewheelcallback(int starting, void *arg){
  (void)arg;

  freewheel_mode = starting;

  if (use_jack_freewheel==true && starting==0) {
    /* wait for buffer to flush to disk */
    while(vringbuffer_reading_size(vringbuffer) > 0)
      msleep(2);
  }
}

#if NEW_JACK_LATENCY_API
static void jack_latency_cb(jack_latency_callback_mode_t mode, void *arg) {
  (void)arg;

  int ch;
  jack_latency_range_t jlty;
  jack_nframes_t max_latency = 0;

  if (mode != JackCaptureLatency) return;

  jack_port_t **ports = ATOMIC_GET(g_ports);

  if (!ports) return;

  for(ch=0;ch<num_channels;ch++) {
    if(ports[ch]==0) continue;
    jack_port_get_latency_range(ports[ch], JackCaptureLatency, &jlty);
    if (jlty.max > max_latency) max_latency= jlty.max;
  }
  j_latency = max_latency;
}
#endif

static void create_ports(void){
  jack_port_t** ports = my_calloc (sizeof (jack_port_t *),num_channels);
  {
    int ch;
    for(ch=0;ch<num_channels;ch++) {
      char name[500];
      sprintf(name,"input%d",ch+1);
      ports[ch]=jack_port_register(client,name,JACK_DEFAULT_AUDIO_TYPE,JackPortIsInput|JackPortIsTerminal,0);
      if(ports[ch]==0){
	print_message("Unable to register input port \"%s\"!\n", name);
	jack_client_close(client);
	exit(1);
      }
    }
  }

  ATOMIC_SET(g_ports, ports);
}






/////////////////////////////////////////////////////////////////////
/////////////////// INIT / WAIT / SHUTDOWN //////////////////////////
/////////////////////////////////////////////////////////////////////


static void finish(int sig){
  (void)sig;
  //turn_on_echo(); //Don't think we can do this from a signal handler...
  SEM_SIGNAL(stop_sem);
}

static void jack_shutdown(void *arg){
  (void)arg;
  fprintf(stderr,"jack_capture: JACK shutdown.\n");
  jack_has_been_shut_down=true;
  SEM_SIGNAL(stop_sem);
}


static jack_client_t * new_jack_client(char *name) {
  // This type is also defined in jack lib
  // returned from several jack operations, formed by OR-ing
  // the relevant status bits
  jack_status_t status;

  // jack_client_open comes from library jack/jack.h
  // JackNoStartServer is an enum declared in jack lib with value 0x01
  // It is basically the code to tell jack to start the server
  // It can be set by an environment variable as well JACK_NO_START_SERVER
  // check jackaudio.org/api/types_8h.html
  jack_client_t *client = jack_client_open(name, JackNoStartServer, &status, NULL);

  if(client == NULL) {
    print_message("jack_client_open() failed, "
		"status = 0x%2.0x\n", status);
    exit(1);
  }

  return client;
}


static void start_jack(void) {
  static bool I_am_already_called=false;

  // start_jack is called more than once
  // if the --port argument has been used.
  if(I_am_already_called)
    return;

  // function new_jack_client defined in this file
  // client is a global variable defined on top
  // with a custom type from jack lib, jack_client_t
  // jackname by default is jack_capture
  client = new_jack_client(jackname);

  // function from jack lib
  // static variable declared on top
  // only set once here (default 48000)
  // number of samples per second!
  jack_samplerate = jack_get_sample_rate(client);

  // function from jack lib
  // this is a static variable defined on top
  // only set once here (default 2048)
  // seems to be the buffer for accumulating entry bytes
  block_size = jack_get_buffer_size(client);
  printf("block size: %d\n", block_size);

  I_am_already_called=true;
}

static pthread_t keypress_thread={0};
static void* keypress_func(void* arg){
  (void)arg;

  char gakk[64];

  turn_off_echo();

  char *fgets_result;

 again:

  fgets_result = fgets(gakk,49,stdin);

  if(timemachine_mode==true && timemachine_recording==false){
    timemachine_recording = true;
    goto again;
  }

  if(fgets_result!=NULL)
    ungetc('\n',stdin);

  program_ended_with_return = true;

  SEM_SIGNAL(stop_sem);
  return NULL;
}

static void start_keypress_thread(void){
  pthread_create(&keypress_thread, NULL, keypress_func, NULL);
}

static const char *advanced_help =
  "jack_capture  [--bitdepth n] [--channels n] [--port port] [filename]\n"
  "              [ -b        n] [ -c        n] [ -p    port]\n"
  "\n"
  "\"bitdepth\" is by default FLOAT. It can be set to either 8, 16, 24 or 32. (for relevant formats)\n"
  "\"channels\" is by default 2.\n"
  "\"port\"     is by default set to the two first physical outputs. The \"port\" argument can be\n"
  "           specified more than once.\n"
  "\"filename\" is by default autogenerated to \"jack_capture_<number>.<format>\"\n"
  "           Beware that you might risk overwriting an old file by using this option.\n"
  "           By not using this option, you will not overwrite an old file.\n"
  "           See also the option --filename-prefix\n"
  "           If the filename has a valid file extension then this will be used to format the output file.\n"
  "           So that \"jack_capture ouput.ogg\" will produce an ogg file, unless a format argument is provided.\n"
  "\n"
  "\n"
  "Additional arguments:\n"
  "[--recording-time n] or [-d n]    -> Recording is stopped after \"n\" seconds.\n"
  "                                    This options also starts jack_capture in --no-stdin mode.\n"
  "                                    To stop recording before the timeout, one can press Ctrl-C.)\n"
  "[--filename-prefix s]/[-fp n]     -> Sets first part of the autogenerated filename.\n"
  "                                    (default is \"jack_capture_\")\n"
  "[--leading-zeros n] or [-z n]     -> \"n\" is the number of zeros to in the autogenerated filename.\n"
  "                                    (-z 2 -> jack_capture_001.wav, and so on.) (default is 1)\n"
  "[--format format] or [-f format]  -> Selects fileformat provided by libsndfile.\n"
  "                                     See http://www.mega-nerd.com/libsndfile/api.html#open\n"
  "                                     This option will overwrite any format specified by the output filename.\n"
  "                                     (Default is wav for 1 or 2 channels, and wavex for more than 2.)\n"
  "[--print-formats] or [-pf]        -> Prints all sound formats provided to sndfile to screen and then\n"
  "                                     exits.\n"
  "[--version] or [-v]               -> Prints out version.\n"
  "[--silent] or [-s]                -> Suppresses some common messages printed to the terminal.\n"
  "[--absolutely-silent] or [-as]    -> Suppresses all messages printed to the terminal.\n"
  "                                     Warning: libraries used by jack_capture may still print messages.\n"
  "[--verbose] or [-V]               -> Prints some extra information to the terminal.\n"
  "[--ogg-quality n] or [-oq n]      -> Set Vorbis quality (0.0 low - 1.0 high)\n"
  "                                     default is set by libvorbis (~0.5)\n"
  "[--write-to-stdout] or [-ws]      -> Writes 16 bit little endian to stdout. (the --format option, the\n"
  "[--disable-meter] or [-dm]        -> Disable console meter.\n"
  "[--hide-buffer-usage] or [-hbu]   -> Disable buffer usage updates in the console.\n"
  "[--disable-console] or [-dc]      -> Disable console updates. Same as \"-dm -hbu\".\n"
  "[--no-stdin] or [-ns]             -> Don't read the console. (i.e pressing return won't stop recording.)\n"
  "[--daemon]                        -> Same as writing \"--no-stdin --absolutely-silent\".\n"
  "[--linear-meter] or [-lm]         -> Use linear scale for the console meter (default is dB scale)\n"
  "[--dB-meter-reference or [-dBr]   -> Specify reference level for dB meter. (default=0)\n"
  "[--meterbridge] or [-mb]          -> Start up meterbridge to monitor recorded sound.\n"
  "[--meterbridge-type] or [-mt]     -> Specify type. vu (default), ppm, dpm, jf or sco.\n"
  "[--meterbridge-reference]/[-mr]   -> Specify reference level for meterbidge.\n"
  "[--jack-transport]/[-jt]          -> Start program, but do not start recording until jack transport has started rolling\n"
  "                                     When jack transport stops, the recording is also stopped, and the program ends.\n"
  "[--jack-transport-multi]/[-jtm]   -> Similar to --jack-transport, but do not end program when jack transport stops.\n"
  "                                     Instead, record to a new file when jack_transport starts rolling again.\n"
  "                                     (not implemented yet)\n"
  "[--jack-freewheeling]/[-jf]       -> Start program, but do not start recording until jack enters freewheeling mode\n"
  "                                     When jack leaves freewheeling, the recording is also stopped, and the program ends.\n"
  "[--jack-name]/[-jn]               -> Set name of this jack_capture instance in the jack patchbay.\n"
  "[--manual-connections]/[-mc]      -> jack_capture will not connect any ports for you. \n"
  "[--bufsize s] or [-B s]           -> Initial/minimum buffer size in seconds. Default is 8 seconds\n"
  "                                     and 4 seconds for all other formats.\n"
  "[--maxbufsize] or [-MB]           -> Maximum buffer size in seconds jack_capture will allocate.\n"
  "                                     Default is 40. (Buffer is automatically increased during\n"
  "                                     recording when needed. But it will never go beyond this size.)\n"
  "[--filename] or [-fn]             -> Specify filename.\n"
  "                                     Beware that you might risk overwriting an old file by using this option.\n"
  "                                     To only set prefix, use --filename-prefix / -fp instead.\n"
  "                                     (It's usually easier to set last argument instead of using this option)\n"
  "[--osc] or [-O]                   -> Specify OSC port number to listen on. see --help-osc\n"
  "[--timestamp] or [-S]             -> create a FILENAME.tme file for each recording, storing\n"
  "                                     the system-time corresponding to the first audio sample.\n"
  "[--rotatefile N] or [-Rf N]       -> force rotate files every N audio-frames.\n"
  "[--hook-open c] or [-Ho c]        -> command to execute on successful file-open. (see below)\n"
  "[--hook-close c] or [-Hc c]       -> command to execute when closing the session. (see below)\n"
  "[--hook-rotate c] or [-Hr c]      -> command to execute on file-name-rotation. (see below)\n"
  "[--hook-timing c] or [-Ht c]      -> callback when first audio frame is received. (see below)\n"
  "[--timemachine] or [-tm]          -> jack_capture operates in \"timemachine\" mode.\n"
  "[--timemachine-prebuffer s]       -> Specify (in seconds) how long time to prebuffer in timemachine mode.\n"
  "[ -tmpb s]                        -> ------------------------ \"\" ----------------------------------------\n"
  "\n"
  " All hook options take a full-path to an executable as argument.\n"
  " The commands are executed in a fire-and-forget style upon internal events.\n"
  " All output of the hooks is discarded.\n"
  " Paramaters passed to the hook-scripts:\n"
  "  open:   CMD <filename>\n"
  "  close:  CMD <filename> <xrun-count> <io-error-count>\n"
  "  rotate: CMD <filename> <xrun-count> <io-error-count> <new-filename> <seq-number>\n"
  "  timing: CMD <filename> <time-sec> <time-nses> <jack-port-latency in frames>\n"
  "\n"
  "Examples:\n"
  "\n"
  "To record a stereo file of what you hear:\n"
  "  $jack_capture\n"
  "\n"
  "To record a stereo file of what you hear in the flac format:\n"
  " $jack_capture -f flac\n"
  "\n"
  "To record a stereo file of what you hear in the ogg format:\n"
  " $jack_capture -f ogg\n"
  "\n"
  "To record a named stereo file of what you hear in the ogg format:\n"
  " $jack_capture output.ogg\n"
  "\n"
  "To record a stereo file of what you hear in the wav format:\n"
  "  $jack_capture --port system:playback_1 --port system:playback_2\n"
  "****************************************************************************\n"
  "**** NOTE! The above example does _exactly_ the same as the default!!!  ****\n"
  "****************************************************************************\n"
  "\n"
  "Same result as above, but using a different syntax:\n"
  "  $jack_capture --channels 2 --port system:playback*\n"
  "\n"
  "To record the output from jamin:\n"
  "  $jack_capture --port jamin:out* sound_from_jamin.wav\n"
  "\n"
  "To record all sound coming in to jamin:\n"
  "  $jack_capture --port jamin:in* sound_to_jamin.wav\n"
  "\n"
  "To record all sound coming in and out of jamin:\n"
  "  $jack_capture --port jamin* sound_to_and_from_jamin.wav\n"
  "\n"
  "To record a stereo file from the soundcard:\n"
  "  $jack_capture -c 2 -p system:capture*\n"
  "\n";

static const char *osc_help =
  "If called with -O <udp-port-number>, jack-capture can be remote-controlled.\n"
	"The following OSC (Open Sound Control) messages are understood:\n"
  "\n"
  "  /jack_capture/stop        (no arguments) -- stop recording and exit\n"
  "  /jack_capture/rotate      (no arguments) -- rotate file-name\n"
  "  /jack_capture/tm/start    (no arguments) -- in timemachine-mode: start recording.\n"
  "  /jack_capture/tm/stop     (no arguments) -- in timemachine-mode: stop recording.\n"
  "\n"
  "Example:\n"
  "  jack_capture -O 7777\n"
	"  oscsend localhost 7777 /jack_capture/stop\n"
  "The 'oscsend' utility comes with liblo, deb-pkg: liblo-utils.\n"
  "\n"
  "Caveat:\n"
	"When used with hook-commands (-Hr, -Hc, etc) the OSC port will be in use\n"
	"until the last of the hook-commands has terminated.\n"
	"Launching a new instance of jack_capture with the same OSC port while some\n"
	"hook-script of a previous instance is still running, will prevent jack_capture\n"
	"from listening on that UDP-port (non fatal - \"port is in use\" warning).\n"
	"jack_capture will work fine, but can not be remote-controlled.\n"
  "\n";

char * usage = "\n"
               "To record what you hear, just run\n"
               "\n"
               "     jack_capture\n"
               "\n"
               "To list advanced options, run\n"
               "\n"
               "     jack_capture --advanced-options (or --help2)\n"
               "\n";

int max_int(int a, int b) {
  return a > b ? a : b;
}

float max_float(float a, float b) {
  return a > b ? a : b;
}

void init_arguments(int argc, char *argv[]) {
  int lokke;

  for (lokke = 0; lokke < argc; lokke++) {
    char *a = argv[lokke];

    if (!strcmp("--help", a) || !strcmp("-h", a)) {
      fprintf(stderr, "%s", usage);
      exit(0);
    } else if (!strcmp("--advanced-options", a) || !strcmp("--help2", a)) {
      printf("%s", advanced_help);
      exit(0);
    } else if (!strcmp("--help-osc", a) || !strcmp("--help3", a)) {
      printf("%s", osc_help);
      exit(0);
    } else if (!strcmp("--bitdepth", a) || !strcmp("-b", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      bitdepth = atoll(argv[++lokke]);
    } else if (!strcmp("--bufsize", a) || !strcmp("-B", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      min_buffer_time = atof(argv[++lokke]);
      min_buffer_time = max_float(0.01, min_buffer_time);
    } else if (!strcmp("--channels", a) || !strcmp("-c", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      num_channels = atoll(argv[++lokke]);
      num_channels = max_int(0, num_channels);
    } else if (!strcmp("--filename-prefix", a) || !strcmp("-fp", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      filename_prefix = argv[++lokke];
    } else if (!strcmp("--leading-zeros", a) || !strcmp("-z", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      leading_zeros = argv[++lokke];
    } else if (!strcmp("--recording-time", a) || !strcmp("-d", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      {
        recording_time       = atof(argv[++lokke]);
        start_jack();
        num_frames_to_record = seconds_to_frames(recording_time);
        no_stdin             = true;
        fixed_duration       = true;
      }
    } else if (!strcmp("--port", a) || !strcmp("-p", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      {
        start_jack();
        portnames_add(argv[++lokke]);
      }
    } else if (!strcmp("--format", a) || !strcmp("-f", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      soundfile_format = argv[++lokke];
      soundfile_format_is_set=true;
    } else if (!strcmp("--version", a) || !strcmp("-v", a)) {
      puts(VERSION);
      exit(0);
    } else if (!strcmp("--silent", a) || !strcmp("-s", a)) {
      silent = true;
    } else if (!strcmp("--debug", a) || !strcmp("-d", a)) {
      DEBUG = true;
    } else if (!strcmp("--absolutelty-silent", a) || !strcmp("-as", a)) {
      absolutely_silent = true;
      use_vu = false;
      silent = true;
      show_bufferusage = false;
    } else if (!strcmp("--verbose", a) || !strcmp("-V", a)) {
      verbose = true;
    } else if (!strcmp("--print-formats", a) || !strcmp("-pf", a)) {
      print_all_formats();
      exit(0);
    } else if (!strcmp("--ogg-quality", a) || !strcmp("-oq", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      ogg_vbr_quality = atof(argv[++lokke]);
      soundfile_format_is_set = true;
      soundfile_format = "ogg";
    } else if (!strcmp("--write-to-stdout", a) || !strcmp("-ws", a)) {
      write_to_stdout = true;
      use_vu = false;
      show_bufferusage = false;
    } else if (!strcmp("--disable-meter", a) || !strcmp("-dm", a)) {
      use_vu = false;
    } else if (!strcmp("--hide-buffer-usage", a) || !strcmp("-hbu", a)) {
      show_bufferusage = false;
    } else if (!strcmp("--disable-console", a) || !strcmp("-dc", a)) {
      use_vu = false;
      show_bufferusage = false;
    } else if (!strcmp("--no_stdin", a) || !strcmp("-ns", a)) {
      no_stdin = true;
    } else if (!strcmp("--daemon", a) || !strcmp("", a)) {
      no_stdin = true;
      absolutely_silent = true;
      use_vu = false;
      silent = true;
      show_bufferusage = false;
    } else if (!strcmp("--linear-meter", a) || !strcmp("-lm", a)) {
      vu_dB = false;
    } else if (!strcmp("--db-meter-reference", a) || !strcmp("-dBr", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

       vu_dB = true;
       vu_bias = powf(10.0f, atof(argv[++lokke]) * -0.05f); // from meterbridge
    } else if (!strcmp("--meterbridge", a) || !strcmp("-mb", a)) {
       use_meterbridge=true;
    } else if (!strcmp("--meterbridge-type", a) || !strcmp("-mt", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

       use_meterbridge = true;
       meterbridge_type = argv[++lokke];
    } else if (!strcmp("--meterbridge-reference", a) || !strcmp("-mr", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      use_meterbridge = true;
      meterbridge_reference = argv[++lokke];
    } else if (!strcmp("--jack-transport", a) || !strcmp("-jt", a)) {
      use_jack_transport = true;
    } else if (!strcmp("--jack-freewheel", a) || !strcmp("-jf", a)) {
      use_jack_freewheel = true;
    } else if (!strcmp("--manual-connections", a) || !strcmp("-mc", a)) {
      use_manual_connections = true;
    } else if (!strcmp("--filename", a) || !strcmp("-fn", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      base_filename = argv[++lokke];
    } else if (!strcmp("--osc", a) || !strcmp("-O", a)) {
#if HAVE_LIBLO
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      osc_port = atoi(argv[++lokke]);
#else
      fprintf(stderr,"osc not supported. liblo was not installed when compiling jack_capture\n");
      exit(3);
#endif
    } else if (!strcmp("--hook-open", a) || !strcmp("-Ho", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      hook_cmd_opened = argv[++lokke];
    } else if (!strcmp("--hook-close", a) || !strcmp("-Hc", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      hook_cmd_closed = argv[++lokke];
    } else if (!strcmp("--hook-rotate", a) || !strcmp("-Hr", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      hook_cmd_rotate = argv[++lokke];
    } else if (!strcmp("--hook-timing", a) || !strcmp("-Ht", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      hook_cmd_timing = argv[++lokke];
    } else if (!strcmp("--timestamp", a) || !strcmp("-S", a)) {
      create_tme_file = true;
    } else if (!strcmp("--rotatefile", a) || !strcmp("-Rf", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      rotateframe = atoll(argv[++lokke]);
    } else if (!strcmp("--timemachine", a) || !strcmp("-tm", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      timemachine_mode = true;
    } else if (!strcmp("--timemachine-prebuffer", a) || !strcmp("-tmpb", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      }

      timemachine_prebuffer = atof(argv[++lokke]);
    } else if (!strcmp("--jack-name", a) || !strcmp("-jn", a)) {
      if (lokke == argc - 1) {
        fprintf(stderr,"Must supply argument for '%s'\n", argv[lokke]);
        exit(0);
      } else if (lokke == argc-1 && argv[lokke][0] != '-') {
        lokke--;
      }

      jackname = argv[++lokke];
      base_filename = argv[++lokke];
    } else {
      fprintf(stderr, "%s", usage);
      exit(-1);
    }
  }

  // Intergrity Checks

  if(use_jack_freewheel == true && use_jack_transport == true){
    fprintf(stderr,"--jack-transport and --jack-freewheel are mutually exclusive options.\n");
    exit(2);
  }

  if(min_buffer_time <= 0.0f)
    min_buffer_time = DEFAULT_MIN_BUFFER_TIME;

  verbose_print("main() determine file format from filename\n");
  // If no format specified try to determine format from the base_filename
  if(!soundfile_format_is_set && base_filename) {
    char *ext = strrchr(base_filename, '.');

    if(ext) {
      ext++; // skip leading .
      soundfile_format = ext;
      soundfile_format_is_set = true;
    }
  }

  if(timemachine_mode==true) {
    min_buffer_time += timemachine_prebuffer;
    max_buffer_time += timemachine_prebuffer;
  }

  verbose_print("main() find default file format\n");
  if(soundfile_format_is_set==false){
    if(num_channels>2)
      soundfile_format=soundfile_format_multi;
    else
      soundfile_format=soundfile_format_one_or_two;
  }


  verbose_print("main() find filename\n");
  // Find filename
  {
    if(base_filename==NULL){
      base_filename=my_calloc(1,5000);
      for(int try=1;try<100000;try++){
        sprintf(base_filename,"%s%0*d.%s",filename_prefix,leading_zeros+1,try,soundfile_format);
        if(access(base_filename,F_OK)) break;
      }
    }
  }
}

char *string_concat(char *s1,char *s2) {
  char *ret=malloc(strlen(s1) + strlen(s2) + 4);
  sprintf(ret,"%s%s",s1,s2);
  return ret;
}

// looking for the character "c" in string "s"
// returns the position (index) of the character "c"
// if not found returns -1
int string_charpos(char *s, char c){
  int pos = 0;

  while(s[pos] != 0){
    if(s[pos] == c)
      return pos;
    pos++;
  }

  return -1;
}

char *substring(char *s, int start, int end){
  char *ret = calloc(1, end - start + 1);
  int read_pos = start;
  int write_pos = 0;

  while(read_pos < end)
    ret[write_pos++] = s[read_pos++];

  return ret;
}

// modifies input.
char *strip_whitespace(char *s){
  // Copy the address of the input string
  // to ret so further pointer arithmetic
  // does not affect the original pointer
  char *ret = s;

  // strip before
  // isspace is a function from stdlib
  // by doing pointer arithmetic
  // increments the address of the pointer tmp
  // the original str address remains unmodified
  //
  // strips white space at the beginning of str
  while(isspace(ret[0]))
    // goes recursively forward
    // basically the pointer variable ret
    // will end up to be the address of the
    // first non white character
    ret++;

  // strip after
  int pos = strlen(ret) - 1;

  // strips white space at the end of str
  while(isspace(ret[pos])) {
    // Sets any white character after the str
    // to be 0 so string is terminated there
    // goes recursively backwards
    ret[pos] = 0;
    pos--;
  }

  return ret;
}

// max size is used as 500
char **read_config(int *argc, int max_size) {
  // Allocates space in memory
  // sizeof(char*) is the size of the address like 0x7ffee6ffa7b0
  // 4 (bytes) in 32 bits environment or 8 (bytes) in 64 bits environment
  // Basically allocating 500 spaces of 8 bytes in memory initilized at zeros
  // we declare argv as double pointer because it will hold memory addresses
  char **argv = calloc(max_size, sizeof(char*));

  // Setting the address of an int to be zero?? Why...
  *argc = 0;

  // if HOME env variable is null we will return the
  // address of the whole memory block created by calloc
  // with only zeros
  if(getenv("HOME") == NULL)
    return argv;

  // getenv will return a pointer to a string
  // this is to open the config file
  FILE *file = fopen(
      string_concat(getenv("HOME"), "/.jack_capture/config"), "r");

  // if no file could be open at that location we return the
  // address of the whole memory block created by calloc
  // with only zeros
  if(file==NULL)
    return argv;

  // Config file was found...
  // allocating 512 bytes of memory in readline
  char *readline = malloc(512);

  // reads line by line of the file
  // why the hardcoded 510 still a mistery
  while(fgets(readline, 510, file) != NULL) {
    char *line = strip_whitespace(readline);

    // checking if the line is empty
    // or if it is a commented line
    if(line[0] == 0 || line[0] == '#')
      continue;

    if(*argc >= max_size - 3) {
      fprintf(stderr, "Too many arguments in config file.\n");
      exit(-2);
    }

    int split_pos = string_charpos(line, '=');

    // if there is no character "=" in this line
    if(split_pos != -1) {
      char *name = strip_whitespace(substring(line, 0, split_pos));
      char *value = strip_whitespace(
        substring(line, split_pos + 1, strlen(line)));

      if(strlen(name) > 0 && strlen(value) > 0){
        argv[*argc] = string_concat("--",name);
        *argc       = *argc + 1;

        if(value[0]=='~')
          value = string_concat(getenv("HOME"), &value[1]);

        argv[*argc] = value;
        *argc = *argc + 1;
      }
    } else {
      argv[*argc] = string_concat("--", line);

      // increment the counter of the number of arguments
      *argc = *argc + 1;
    }
  }

  return argv;
}

void init_various(void) {
  verbose_print("main() init jack 1\n");
  // Init jack 1
  {
    if(use_manual_connections == false) {
      // inits a semaphore
      // starts an infinite loop
      // which locks the semaphore
      start_connection_thread();
    }

    start_jack();
    portnames_add_defaults();
  }

  verbose_print("main() init buffers\n");
  // Init buffers
  {
    buffers_init();
  }

  if (access(base_filename, F_OK)==0){
    print_message("\n");
    print_message("!!! Warning, Overwriting existing file %s !!!\n", base_filename);
    print_message("\n");
  }

  verbose_print("main() Open soundfile and setup disk callback.\n");
  // Open soundfile and start disk thread
  {
    if(!open_soundfile()){
      jack_client_close(client);
      exit(-2);
    }

    vringbuffer_set_receiver_callback(vringbuffer, disk_callback);
  }

  verbose_print("main() Init waiting.\n");
  // Init waiting.
  {
    SEM_INIT(stop_sem);

    signal(SIGINT,finish);
    signal(SIGTERM,finish);

    if(no_stdin==false)
      start_keypress_thread();
  }

  verbose_print("main() Init jack 2.\n");
  // Init jack 2
  {
    jack_set_process_callback(client, process, NULL);
    jack_set_xrun_callback(client, xrun, NULL);

    jack_on_shutdown(client, jack_shutdown, NULL);

#if NEW_JACK_LATENCY_API
    jack_set_latency_callback (client, jack_latency_cb, NULL);
#endif

    if(use_manual_connections==false)
      jack_set_graph_order_callback(client,graphordercallback,NULL);

    jack_set_freewheel_callback(client,freewheelcallback,NULL);

    if (jack_activate(client)) {
      fprintf (stderr,"\nCan not activate client");
      exit(-2);
    }

    create_ports();
    if(use_manual_connections==false)
      connect_ports(ATOMIC_GET(g_ports));
  }


  verbose_print("main() Everything initialized.\n");
  // Everything initialized.
  //   (The threads are waiting for this variable, not the other way around, so now it just needs to be set.)
  {
    ATOMIC_SET(is_initialized, true);
    wake_up_connection_thread(); // Usually (?) not necessarry, but just in case.
  }

  verbose_print("main() Start meterbridge.\n");
  // Start meterbridge
  {
    if(use_meterbridge)
      start_meterbridge(num_channels);
  }

  verbose_print("main() Print some info.\n");
  // Print some info
  {
    if(fixed_duration==true){
      if(silent==false)
        print_message(
                    "Recording to \"%s\". The recording is going\n"
                    MESSAGE_PREFIX "to last %lf seconds Press <Ctrl-C> to stop before that.\n",
                    base_filename,
                    recording_time);
    }else{
      if(silent==false) {
        if (timemachine_mode==true) {
          print_message("Waiting to start recording of \"%s\"\n",base_filename);
          print_message("Press <Ctrl-C> to stop recording and quit.\n");
        }else
          print_message("Recording to \"%s\". Press <Return> or <Ctrl-C> to stop.\n",base_filename);
        //fprintf(stderr,"Recording to \"%s\". Press <Return> or <Ctrl-C> to stop.\n",base_filename);
      }
    }
  }

  verbose_print("main() Start helper thread.\n");
  // Start the helper thread, which takes care of the console
  {
    setup_helper_thread();
  }

  //noecho();

}


void wait_until_recording_finished(void){
  verbose_print("main() Wait.\n");

  if(use_jack_transport==true)
    print_message("Waiting for JackTransportRolling.\n");
  if(use_jack_freewheel==true)
    print_message("Waiting for Jack Freewheeling .\n");


  SEM_WAIT(stop_sem);

  turn_on_echo();
  if(helper_thread_running==1){
    //      if(use_vu || show_bufferusage)
    //	printf("%c[%dA",0x1b,1); // Pressing return moves the cursor.
    if(silent==false){  // messy...
      print_message("Please wait while writing all data to disk. (shouldn't take long)\n");
      msleep(2);
    }
    //print_message("%c[%dAPlease wait while writing all data to disk. (shouldn't take long)\n",0x1b,1); // necessary.
  }
}



void stop_recording_and_cleanup(void){
  verbose_print("main() Stop recording and clean up.\n");

  __atomic_store_n (&is_running_atomic, false, __ATOMIC_SEQ_CST);

  if(use_manual_connections==false)
    stop_connection_thread();

  vringbuffer_stop_callbacks(vringbuffer); // Called before cleanup_disk to make sure all data are sent to the callback.

  cleanup_disk();

  if(use_meterbridge)
    kill(meterbridge_pid,SIGINT);

  stop_helper_thread();

#if HAVE_LIBLO
  shutdown_osc();
#endif

  if(jack_has_been_shut_down==false)
    jack_client_close(client);

  if(silent==false){
    usleep(50); // wait for terminal
    fprintf(stderr,"%c[31m",0x1b); //red color
    fprintf(stderr,"Finished.");
    fprintf(stderr,"%c[0m",0x1b); // reset colors
    fprintf(stderr,"\n");
  }
}


void append_argv(char **v1, const char **v2, int len1, int len2, int max_size) {
  int write_pos = len1;
  int read_pos  = 0;

  if(len1 + len2 >= max_size) {
    fprintf(stderr,"Too many arguments.\n");
    exit(-3);
  }

  while(write_pos < len1 + len2)
    v1[write_pos++] = (char*)v2[read_pos++];
}

void display_parameters() {
  // "--advanced-options"
  // "--advanced-options"
  // "--help-osc"
  // "--bitdepth"
  printf("bitdepth: %d\n", bitdepth);
  // "--bufsize"
  printf("min_buffer_time: %f\n", min_buffer_time);
  // "--channels"
  printf("num_channels: %d\n", num_channels);
  // "--filename-prefix"
  printf("filename_prefix: %s\n", filename_prefix);
  // "--leading-zeros"
  printf("leading_zeros: %d\n", leading_zeros);
  // "--recording-time"
  printf("recording_time: %f\n", recording_time);
  // "--port"
  // portnames_add(argv[++lokke]);
  // "--format"
  printf("soundfile_format: %s\n", soundfile_format);
  printf("soundfile_format_is_set: %d\n", soundfile_format_is_set);
  // "--version"
  // puts(VERSION);
  // "--silent"
  printf("silent: %d\n", silent);
  // "--debug"
  printf("DEBUG: %d\n", DEBUG);
  // "--absolutelty-silent"
  printf("absolutely_silent: %d\n", absolutely_silent);
  // "--verbose"
  printf("verbose: %d\n", verbose);
  // "--print-formats"
  // print_all_formats();
  // "--ogg-quality"
  printf("ogg_vbr_quality: %f\n", ogg_vbr_quality);
  // "--write-to-stdout"
  printf("write_to_stdout: %d\n", write_to_stdout);
  // "--use_vu --disable-meter"
  printf("use_vu: %d\n", use_vu);

  printf("num_frames_to_record: %lld\n", num_frames_to_record);
  // "--no_stdin"
  printf("no_stdin: %d\n", no_stdin);
  printf("fixed_duration: %d\n", fixed_duration);
  printf("show_bufferusage: %d\n", show_bufferusage);
  // "--linear-meter"
  printf("vu_dB: %d\n", vu_dB);
  // "--db-meter-reference"
  printf("vu_bias: %f\n", vu_bias);
  // "--meterbridge-type"
  printf("use_meterbridge: %d\n", use_meterbridge);
  printf("meterbridge_type: %s\n", meterbridge_type);
  printf("meterbridge_reference: %s\n", meterbridge_reference);
  // "--jack-transport"
  printf("use_jack_transport: %d\n", use_jack_transport);
  // "--jack-freewheel"
  printf("use_jack_freewheel: %d\n", use_jack_freewheel);
  // "--manual-connections"
  printf("use_manual_connections: %d\n", use_manual_connections);
  // "--filename"
  printf("base_filename: %s\n", base_filename);
  // "--osc"
  printf("osc_port: %d\n", osc_port);
  // "--hook-open"
  printf("hook_cmd_opened: %s\n", hook_cmd_opened);
  // "--hook-close"
   printf("hook_cmd_closed: %s\n", hook_cmd_closed);
   // "--hook-rotate"
   printf("hook_cmd_rotate: %s\n", hook_cmd_rotate);
   // "--hook-timing"
   printf("hook_cmd_timing: %s\n", hook_cmd_timing);
   // "--timestamp"
   printf("create_tme_file: %d\n", create_tme_file);
   // "--rotatefile"
   printf("rotateframe: %lld\n", rotateframe);
   // "--timemachine"
   printf("timemachine_mode: %d\n", timemachine_mode);
   // "--timemachine-prebuffer"
   printf("timemachine_prebuffer: %f\n", timemachine_prebuffer);
   // "--jack-name"
   printf("jackname: %s\n", jackname);
}


int main (int argc, char *argv[]){
  g_thread_type = MAIN_THREAD;

  // Here we just redeclare the array of pointers
  // as a double star
  char **org_argv = argv;

  // Remove executable name, which is the arguments first element.
  // Ampersand captures the mem address of the first element
  // in the array. We then assign this pointer to argv
  // effectively removing the first element from the array
  argv = &argv[1];

  // substract one from the number of arguments
  // to be consisent
  argc = argc - 1;

  // c_carts stands for config
  // get arguments both from command line and config file
  // (config file is read first, so that command line can override)
  int config_argc;

  // we pass 500 arbitrarely as the max_size
  // of the read config function
  // basically all options set in the by the command line are added
  // to the config_argv pointer as if they were writen in the file
  char **config_argv = read_config(&config_argc, 500);
  append_argv(config_argv, (const char**)argv, config_argc, argc, 500);

  init_arguments(config_argc + argc, config_argv);

  if (DEBUG) display_parameters();

#if HAVE_LIBLO
  if (init_osc(osc_port)) {
    /* no OSC available */
    osc_port=-1;
  }
#endif

  init_various();

  wait_until_recording_finished();

  stop_recording_and_cleanup();

  if (timemachine_mode == true && program_ended_with_return == true) {
    execvp (org_argv[0], (char *const *) org_argv);
    print_message("Error: exec returned: %s.\n", strerror(errno));
    exit(127);
  }

  return 0;
}

