#include "InputHandler.h"
#include "Logger.h"
#include "Timer.h"


InputHandler::InputHandler(const string main_input_path, const string backup_input_path)
{
    this->main_input_path = main_input_path;
    this->backup_input_path = backup_input_path;
}

InputHandler::~InputHandler(){
    avformat_free_context(main_input_ctx);
    avformat_free_context(backup_input_ctx);
}

void InputHandler::open_main_input()
{
    Timer init_time_check;
    int ret = avformat_open_input(&main_input_ctx, main_input_path.c_str(), nullptr, nullptr);
    BOOST_LOG(debug) << "[INPUT] avformat_open_input time : " << init_time_check.elapsed();

    init_time_check.reset();

    if(ret < 0)
    {
        Logger::get_instance().print_log_with_reason(AV_LOG_ERROR,"avformat main open failed", ret);
        return;
    }

    avformat_find_stream_info(main_input_ctx, nullptr);
    BOOST_LOG(debug) << "[INPUT] avformat_find_stream_info time : " << init_time_check.elapsed();
}

void InputHandler::open_backup_input()
{
    int ret = avformat_open_input(&backup_input_ctx, backup_input_path.c_str(), nullptr, nullptr);
    if(ret < 0)
    {
        Logger::get_instance().print_log_with_reason(AV_LOG_ERROR,"avformat backup open failed", ret);
        return;
    }
    avformat_find_stream_info(backup_input_ctx, nullptr);
}

vector<AVStream*> InputHandler::get_main_video_stream()
{
    vector<AVStream*> video_streams;
    for (unsigned int i = 0; i < main_input_ctx->nb_streams; i++) {
        if (main_input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_streams.push_back(main_input_ctx->streams[i]);
        }
    }
    return video_streams;
}

vector<AVStream*> InputHandler::get_backup_video_stream()
{
    vector<AVStream*> video_streams;
    for (unsigned int i = 0; i < backup_input_ctx->nb_streams; i++) {
        if (backup_input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_streams.push_back(backup_input_ctx->streams[i]);
        }
    }
    
    return video_streams;
}

AVFormatContext* InputHandler::get_main_input_context()
{
    open_main_input();
    return main_input_ctx;
}

AVFormatContext* InputHandler::get_backup_input_context()
{
    open_backup_input();
    return backup_input_ctx;
}