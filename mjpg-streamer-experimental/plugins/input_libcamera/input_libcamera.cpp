/*******************************************************************************
#                                                                              #
# Libcamera input plugin                                                       #
# Copyright (C) 2016 Dustin Spicuzza                                           #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <dlfcn.h>
#include <pthread.h>
#include <linux/videodev2.h>
#include <fcntl.h>

#include "input_libcamera.h"
#include "LibCamera.h"
#include "jpeg_utils.h"

using namespace std;

/* private functions and variables to this plugin */
static globals     *pglobal;

typedef struct {
    int fps_set, fps,
        quality_set, quality,
        buffercount_set, buffercount,
        co_set, co,
        br_set, br,
        sa_set, sa,
        gain_set, gain,
        ex_set, ex,
        afmode_set, afmode,
        afrange_set, afrange,
        lensposition_set, lensposition,
        camera_set, camera,
        rotation_set, rotation;
} context_settings;

typedef struct {
    pthread_t   worker;
    pthread_mutex_t control_mutex;
    LibCamera camera;
    context_settings *init_settings;
    struct vdIn *videoIn;
} context;

static const struct {
  const char * k;
  const int v;
} autofocus_mode[] = {
  { "manual", controls::AfModeManual },
  { "auto", controls::AfModeAuto },
  { "continuous", controls::AfModeContinuous },
};

static const struct {
  const char * k;
  const int v;
} autofocus_range[] = {
  { "normal", controls::AfRangeNormal },
  { "macro", controls::AfRangeMacro },
  { "full", controls::AfRangeFull },
};

void *worker_thread(void *);
void worker_cleanup(void *);

#define INPUT_PLUGIN_NAME "Libcamera Input plugin"
static char plugin_name[] = INPUT_PLUGIN_NAME;


static void help() {
    
    fprintf(stderr,
    " ---------------------------------------------------------------\n" \
    " Help for input plugin..: " INPUT_PLUGIN_NAME "\n" \
    " ---------------------------------------------------------------\n" \
    " The following parameters can be passed to this plugin:\n\n" \
    " [-r | --resolution ]...: the resolution of the video device,\n" \
    "                          can be one of the following strings:\n" \
    "                          ");
    
    resolutions_help("                          ");
    
    fprintf(stderr,
    " [-c | --camera ].......: Chooses the camera to use.\n"
    " [-f | --fps ]..........: frames per second\n" \
    " [-b | --buffercount ]...: Set the number of request buffers.\n"  \
    " [-q | --quality ] .....: set quality of JPEG encoding\n" \
    " [-rot | --rotation]....: Request an image rotation, 0 or 180\n"
    " [-s | --snapshot ] .....: Set the snapshot resolution, if not set will default to the maximum resolution.\n" \
    " ---------------------------------------------------------------\n" \
    " Optional parameters (may not be supported by all cameras):\n\n"
    " [-br ].................: Set image brightness (integer)\n"\
    " [-co ].................: Set image contrast (integer)\n"\
    " [-sa ].................: Set image saturation (integer)\n"\
    " [-ex ].................: Set exposure (integer)\n"\
    " [-gain ]...............: Set gain (integer)\n"
    " [-afmode]..............: Control to set the mode of the AF (autofocus) algorithm.(manual, auto, continuous)\n"
    " [-afrange].............: Set the range of focus distances that is scanned.(normal, macro, full)\n"
    " [-lensposition]........: Set the lens to a particular focus position, expressed as a reciprocal distance (0 moves the lens to infinity), or \"default\" for the hyperfocal distance"
    " ---------------------------------------------------------------\n\n"\
    );
}

static context_settings* init_settings() {
    context_settings *settings;
    
    settings = (context_settings*)calloc(1, sizeof(context_settings));
    if (settings == NULL) {
        IPRINT("error allocating context");
        exit(EXIT_FAILURE);
    }
    
    settings->quality = 80;
    return settings;
}

/*** plugin interface functions ***/

/******************************************************************************
Description.: parse input parameters
Input Value.: param contains the command line string and a pointer to globals
Return Value: 0 if everything is ok
******************************************************************************/

int input_init(input_parameter *param, int plugin_no)
{   
    // char *device = "/dev/video0";
    char *device_id;
    int width = 640, height = 480, i, device_idx;
    int snapshot_width = 0, snapshot_height = 0;
    
    input * in;
    context *pctx;
    context_settings *settings;
    int ret;
    ControlList controls_(controls::controls);
    int64_t frame_time;
    bool controls_flag = false;
    
    pctx = new context();
    
    settings = pctx->init_settings = init_settings();
    pglobal = param->global;
    in = &pglobal->in[plugin_no];
    in->context = pctx;

    control libcamera_AfTrigger_ctrl;
	libcamera_AfTrigger_ctrl.group = IN_CMD_GENERIC;
	libcamera_AfTrigger_ctrl.ctrl.id = 1;
	libcamera_AfTrigger_ctrl.ctrl.type = V4L2_CTRL_TYPE_BUTTON;
	strcpy((char*)libcamera_AfTrigger_ctrl.ctrl.name, "AfTrigger");


	in->in_parameters = (control*)malloc((in->parametercount + 1) * sizeof(control));
	in->in_parameters[in->parametercount] = libcamera_AfTrigger_ctrl;
	in->parametercount++;

    param->argv[0] = plugin_name;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    /* parse the parameters */
    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0},                   // 0
            {"help", no_argument, 0, 0},                // 1
            {"r", required_argument, 0, 0},             // 2
            {"resolution", required_argument, 0, 0},    // 3
            {"f", required_argument, 0, 0},             // 4
            {"fps", required_argument, 0, 0},           // 5
            {"q", required_argument, 0, 0},             // 6
            {"quality", required_argument, 0, 0},       // 7
            {"co", required_argument, 0, 0},            // 8
            {"br", required_argument, 0, 0},            // 9
            {"sa", required_argument, 0, 0},            // 10
            {"gain", required_argument, 0, 0},          // 11
            {"ex", required_argument, 0, 0},            // 12
            {"b", required_argument, 0, 0},             // 13
            {"buffercount", required_argument, 0, 0},   // 14
            {"s", required_argument, 0, 0},             // 15
            {"snapshot", required_argument, 0, 0},      // 16
            {"afmode", required_argument, 0, 0},        // 17
            {"afrange", required_argument, 0, 0},       // 18
            {"lensposition", required_argument, 0, 0},  // 19
            {"camera", required_argument, 0, 0},        // 20
            {"rotation", required_argument, 0, 0},      // 21
            {0, 0, 0, 0}
        };
    
        /* parsing all parameters according to the list above is sufficent */
        c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);

        /* no more options to parse */
        if(c == -1) break;

        /* unrecognized option */
        if(c == '?') {
            help();
            return 1;
        }

        /* dispatch the given options */
        switch(option_index) {
        /* h, help */
        case 0:
        case 1:
            help();
            return 1;
        /* r, resolution */
        case 2:
        case 3:
            DBG("case 2,3\n");
            parse_resolution_opt(optarg, &width, &height);
            break;
        /* f, fps */
        case 4:
        OPTION_INT(5, fps)
            break;
        /* q, quality */
        case 6:
        OPTION_INT(7, quality)
            settings->quality = MIN(MAX(settings->quality, 0), 100);
            break;
        OPTION_INT(8, co)
            break;
        OPTION_INT(9, br)
            break;
        OPTION_INT(10, sa)
            break;
        OPTION_INT(11, gain)
            break;
        OPTION_INT(12, ex)
            break;
        /* b, buffercount */
        case 13:
        OPTION_INT(14, buffercount)
            break;
        /* s, snapshot */
        case 15:
        case 16:
            parse_resolution_opt(optarg, &snapshot_width, &snapshot_height);
            break;
        OPTION_MULTI(17, afmode, autofocus_mode)
            break;
        OPTION_MULTI(18, afrange, autofocus_range)
            break;
        OPTION_INT(19, lensposition)
            break;
        OPTION_INT(20, camera)
            break;
        OPTION_INT(21, rotation)
            break;
        default:
            help();
            return 1;
        }
    }

    IPRINT("Desired Resolution: %i x %i\n", width, height);

    if (!settings->buffercount) {
        settings->buffercount = 4;
    } else {
        settings->buffercount = MAX(settings->buffercount, 1);
    }
    ret = pctx->camera.initCamera(settings->camera);
    if (ret) {
        IPRINT("LibCamera::initCamera() failed\n");
        goto fatal_error;
    }
    pctx->camera.configureStill(width, height, formats::BGR888, settings->buffercount, settings->rotation);
    device_id = pctx->camera.getCameraId();
    in->name = (char*)malloc((strlen(device_id) + 1) * sizeof(char));
    sprintf(in->name, device_id);

    if (settings->fps){
        frame_time = 1000000 / settings->fps;
        // Set frame rate
	    controls_.set(controls::FrameDurationLimits, libcamera::Span<const int64_t, 2>({ frame_time, frame_time }));
        controls_flag = true;
    }
    if (settings->br) {
        controls_.set(controls::Brightness, settings->br);
        controls_flag = true;
    }
    if (settings->co) {
        controls_.set(controls::Contrast, settings->co);
        controls_flag = true;
    }
    if (settings->sa) {
        controls_.set(controls::Saturation, settings->sa);
        controls_flag = true;
    }
    if (settings->gain) {
        controls_.set(controls::AnalogueGain, settings->gain);
        controls_flag = true;
    }
    if (settings->ex) {
        controls_.set(controls::ExposureTime, settings->ex);
        controls_flag = true;
    }
    if (settings->afmode) {
        if (settings->lensposition)
            controls_.set(controls::AfMode, controls::AfModeManual);
        else
            controls_.set(controls::AfMode, settings->afmode);
        if (settings->afmode == controls::AfModeAuto)
            controls_.set(controls::AfTrigger, controls::AfTriggerStart);
        controls_flag = true;
    }
    if (settings->afrange) {
        controls_.set(controls::AfRange, settings->afrange);
        controls_flag = true;
    }
    if (settings->lensposition) {
        controls_.set(controls::LensPosition, settings->lensposition);
        controls_flag = true;
    }
    if (controls_flag) {
        pctx->camera.set(controls_);
    }

    pctx->videoIn = (struct vdIn*)malloc(sizeof(struct vdIn));
    if (pctx->videoIn == NULL) {
        IPRINT("error allocating context\n");
        goto fatal_error;
    }
    pctx->videoIn->width = width;
    pctx->videoIn->height = height;
    pctx->videoIn->snapshot_width = snapshot_width;
    pctx->videoIn->snapshot_height = snapshot_height;
    
    return 0;
    
fatal_error:
    worker_cleanup(in);
    closelog();
    exit(EXIT_FAILURE);
}


/******************************************************************************
Description.: stops the execution of the worker thread
Input Value.: -
Return Value: 0
******************************************************************************/
int input_stop(int id)
{
    input * in = &pglobal->in[id];
    context *pctx = (context*)in->context;

    free(pctx->videoIn);

    free(in->buf);
    in->buf = NULL;
    in->size = 0;

    if (pctx != NULL) {
        DBG("will cancel input thread\n");
        pthread_cancel(pctx->worker);
    }
    return 0;
}

/******************************************************************************
Description.: starts the worker thread and allocates memory
Input Value.: -
Return Value: 0
******************************************************************************/
int input_run(int id)
{
    input * in = &pglobal->in[id];
    context *pctx = (context*)in->context;

    in->buf = (uint8_t *)malloc(pctx->videoIn->width * pctx->videoIn->height);
    in->size = 0;
    
    if(pthread_create(&pctx->worker, 0, worker_thread, in) != 0) {
        worker_cleanup(in);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }
    pthread_detach(pctx->worker);

    return 0;
}

/******************************************************************************
Description.: switch image resolution.
Input Value.: -
Return Value: -
******************************************************************************/
void switch_resolution(input *in, uint32_t width, uint32_t height, uint32_t buffercount)
{
    context *pctx = (context*)in->context;
    int ret = pctx->camera.resetCamera(width, height, formats::BGR888, buffercount, 0);
    if (ret) {
        IPRINT("switch_resolution(): Failed to switch camera resolution.\n");
        goto fatal_error;
    }
    pctx->camera.VideoStream(&pctx->videoIn->width, &pctx->videoIn->height, &pctx->videoIn->stride);
    in->buf = (uint8_t *)realloc(in->buf, pctx->videoIn->width * pctx->videoIn->height);
    if (in->buf == NULL) {
        IPRINT("error allocating context\n");
        goto fatal_error;
    }
    return;
fatal_error:
    worker_cleanup(in);
    closelog();
    exit(EXIT_FAILURE);
}

void *worker_thread(void *arg)
{
    input * in = (input*)arg;
    context *pctx = (context*)in->context;
    context_settings *settings = (context_settings*)pctx->init_settings;
    int quality = settings->quality;
    LibcameraOutData frameData;
    int is_switch = 0;

    /* Save the image initial settings. */
    uint32_t width = pctx->videoIn->width, height = pctx->videoIn->height;
    uint32_t buffercount = settings->buffercount;

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, arg);
    
    free(settings);
    pctx->init_settings = NULL;
    settings = NULL;

    pctx->camera.startCamera();
    pctx->camera.VideoStream(&pctx->videoIn->width, &pctx->videoIn->height, &pctx->videoIn->stride);
    while (!pglobal->stop) {

        if (in->snapshot) {
            if (width != pctx->videoIn->snapshot_width || height != pctx->videoIn->snapshot_height) {
                /* The image resolution width and height are set to 0, and the maximum resolution will be used. */
                switch_resolution(in, pctx->videoIn->snapshot_width, pctx->videoIn->snapshot_height, 1);
                is_switch = 1;
            }
            in->snapshot = 0;
        }

        if (!pctx->camera.readFrame(&frameData))
            continue;
            
        pthread_mutex_lock(&in->db);

        pctx->videoIn->framebuffer = frameData.imageData;
        pctx->videoIn->formatIn = V4L2_PIX_FMT_RGB24;
        in->size = compress_image_to_jpeg(pctx->videoIn, in->buf, frameData.size, quality);

        /* signal fresh_frame */
        pthread_cond_broadcast(&in->db_update);
        pthread_mutex_unlock(&in->db);

        pctx->camera.returnFrameBuffer(frameData);
        if (is_switch) {
            switch_resolution(in, width, height, buffercount);
            is_switch = 0;
        }
    }

    pctx->camera.stopCamera();
    pctx->camera.closeCamera();

    IPRINT("leaving input thread, calling cleanup function now\n");
    pthread_cleanup_pop(1);

    return NULL;
}

/******************************************************************************
Description.: this functions cleans up allocated resources
Input Value.: arg is unused
Return Value: -
******************************************************************************/
void worker_cleanup(void *arg)
{
    input * in = (input*)arg;
    if (in->context != NULL) {
        context *pctx = (context*)in->context;
        delete pctx;
        in->context = NULL;
    }
}

/******************************************************************************
Description.: process commands, allows to set libcamera controls
Input Value.: -
Return Value: depends in the command, for most cases 0 means no errors and
              -1 signals an error. This is just rule of thumb, not more!
******************************************************************************/
int input_cmd(int plugin, unsigned int control_id, unsigned int typecode, int value)
{
    input * in = &pglobal->in[plugin];
    context *pctx = (context*)in->context;

    DBG("Requested cmd (id: %d) for the %d plugin. Group: %d value: %d\n", control_id, plugin_number, group, value);
    int i;
    switch(typecode)
    {
        case IN_CMD_GENERIC:
            for(i = 0; i < in->parametercount; i++)
			{
				if((in->in_parameters[i].ctrl.id == control_id) && (in->in_parameters[i].group == IN_CMD_GENERIC))
				{
					DBG("Generic control found (id: %d): %s\n", control_id, in->in_parameters[i].ctrl.name);
                    pthread_mutex_lock(&pctx->control_mutex);
                    ControlList controls_;
					if(control_id == 1)
					{
                        controls_.set(controls::AfMode, controls::AfModeAuto);
						controls_.set(controls::AfTrigger, controls::AfTriggerStart);
					} 
                    pctx->camera.set(controls_);
                    pthread_mutex_unlock(&pctx->control_mutex);
                    DBG("New %s value: %d\n", in->in_parameters[i].ctrl.name, value);
					return 0;
				}
			}
			DBG("Requested generic control (%d) did not found\n", control_id);
			return -1;
            break;
    }

    return 0;
}
