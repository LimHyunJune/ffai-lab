#pragma once
extern "C" {
    #include <libavformat/avformat.h>
	#include <libavcodec/avcodec.h>
}

#include <string>

using namespace std;

class Logger
{
    public:
        void print_log_with_reason(int level,string log,int ret);
        void print_log(int level,string log);
        static Logger& get_instance()
        {
            static Logger logger;
            return logger;
        }
};

#include <boost/log/core.hpp>
#include <boost/log/attributes/clock.hpp>
#include <boost/log/attributes/current_thread_id.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <iostream>
#include <iomanip>

extern boost::log::sources::severity_logger<int> verbose;
extern boost::log::sources::severity_logger<int> debug;
extern boost::log::sources::severity_logger<int> info;
extern boost::log::sources::severity_logger<int> warning;
extern boost::log::sources::severity_logger<int> error;
extern boost::log::sources::severity_logger<int> fatal;

using text_sink = boost::log::sinks::asynchronous_sink<boost::log::sinks::text_ostream_backend>;

struct NoDelete
{
	void operator()(void*) {}
};

extern void init_log();