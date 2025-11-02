#pragma once
extern "C" {
    #include <libavformat/avformat.h>
	#include <libavcodec/avcodec.h>
}
#include <string>
#include <vector>
using namespace std;

class InputHandler
{
    private:
        AVFormatContext* main_input_ctx = nullptr;
        AVFormatContext* backup_input_ctx = nullptr;
        
        string main_input_path;
        string backup_input_path;

        void open_main_input();
        void open_backup_input();
    public:
        InputHandler() = delete;
        InputHandler(const string main_input_path, const string backup_input_path);
        ~InputHandler();
        
        vector<AVStream*> get_main_video_stream();
        vector<AVStream*> get_backup_video_stream();
        
        AVFormatContext* get_main_input_context();
        AVFormatContext* get_backup_input_context();
};