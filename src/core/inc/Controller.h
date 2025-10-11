#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
}

#include <thread>
#include <atomic>
#include <iostream>
#include <chrono>
#include <typeinfo>
#include "InputHandler.h"
#include "DecoderHandler.h"
#include "FilterHandler.h"
#include "EncoderHandler.h"
#include "OutputHandler.h"

#include "TSQueue.h"
#include "Logger.h"

#include <string>
#include <map>
#include <atomic>
#include <mutex>
#include <vector>
using namespace std;

struct ControllerConfig
{
    string main_input_path;
    string backup_input_path;
    FilterConfig filter_config;
    EncoderConfig encoder_config;
    OutputConfig output_config;
};

class Controller
{
    private:
        InputHandler* input_handler = nullptr;
        DecoderHandler* decoder_handler = nullptr;
        FilterHandler* filter_handler = nullptr;
        EncoderHandler* encoder_handler = nullptr;
        OutputHandler* output_handler = nullptr;

        AVFormatContext* main_input_ctx = nullptr;
        vector<AVStream*> main_video_streams;

        AVFormatContext* backup_input_ctx = nullptr;
        vector<AVStream*> backup_video_streams;

        AVFormatContext* active_input_ctx = nullptr;
        vector<AVStream*> active_video_streams;

        atomic<bool> flag;
        
        // input switch
        atomic<bool> main_alive{true};
        atomic<bool> main_recovered{false};
        atomic<bool> backup_alive{true};
        atomic<bool> backup_recovered{false};
        atomic<bool> both_dead{false};
        atomic<bool> stop_reconnected{false};

        static int interrupt_cb(void* apaque);
        atomic<int64_t> last_io_ts_us{0};
        int64_t io_timeout_us = 1500000;

        mutex switch_mtx;
        thread reconnect_thread;

        map<int,AVCodecContext*> dec_ctxs;

        map<int,AVFilterContext*> buffersrc_ctxs;
        AVFilterContext* buffersink_ctx = nullptr;

        AVCodecContext* enc_ctx = nullptr;
        AVFormatContext* out_fmt_ctx = nullptr;
        AVStream* output_stream = nullptr;

        AVBufferRef* hw_device_ctx = nullptr;
        AVBufferRef* hw_frames_ctx = nullptr;

        TSQueue<AVPacket*> input_queue;
        TSQueue<AVFrame*> decoder_queue;
        TSQueue<AVFrame*> filter_queue;
        TSQueue<AVPacket*> encoder_queue;

        std::thread input_thread;
        std::thread decoder_thread;
        std::thread filter_thread;
        std::thread encoder_thread;
        std::thread output_thread;

        bool initialize();
        bool init_hw_device();
        bool init_input();
        bool init_decoder();
        bool init_filter();
        bool init_encoder();
        bool init_output();

        void run_input_thread();
        void run_decoder_thread();
        void run_filter_thread();
        void run_encoder_thread();
        void run_output_thread();

        Controller();
        ~Controller();

        ControllerConfig controller_config;

        void create_streaming_pipeline();
        

    public:
        static Controller& get_instance()
        {
            static Controller instance;
            return instance;
        }
        void start(string req, string target);

};