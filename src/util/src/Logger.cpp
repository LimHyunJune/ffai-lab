#include "Logger.h"

void Logger::print_log_with_reason(int level, string log, int ret)
{
    log += " : %s\n";
    char errorStr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_make_error_string(errorStr, AV_ERROR_MAX_STRING_SIZE, ret);
    av_log(nullptr, level, log.c_str(), errorStr);
}

void Logger::print_log(int level, string log)
{
    av_log(nullptr, level, log.c_str());
}

boost::log::sources::severity_logger<int> verbose(0); // Dominating output
boost::log::sources::severity_logger<int> debug(1);   // Follow what is happening
boost::log::sources::severity_logger<int> info(2);    // Should be informed about
boost::log::sources::severity_logger<int> warning(3); // Strange events
boost::log::sources::severity_logger<int> error(4);   // Recoverable errors
boost::log::sources::severity_logger<int> fatal(5);   // Unrecoverable errors

BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", int)
BOOST_LOG_ATTRIBUTE_KEYWORD(thread_id, "ThreadID", boost::log::attributes::current_thread_id::value_type)

void init_log()
{
	boost::shared_ptr<text_sink> sink = boost::make_shared<text_sink>();

	boost::shared_ptr<std::ostream> stream{ &std::cout, NoDelete{} };

	sink->locked_backend()->add_stream(stream);
	sink->set_filter(severity >= 0);

    sink->set_formatter([message = "Message", severity = "Severity"](const boost::log::record_view &view, boost::log::formatting_ostream &os)
                        {
                            auto log_level = view.attribute_values()[severity].extract<int>().get();
                            std::string_view log_type;
                            switch (log_level)
                            {
                            case 0:
                                log_type = "verbose";
                                break;
                            case 1:
                                log_type = "debug";
                                break;
                            case 2:
                                log_type = "info";
                                break;
                            case 3:
                                log_type = "warning";
                                break;
                            case 4:
                                log_type = "error";
                                break;
                            case 5:
                                log_type = "fatal";
                                break;
                            };

                            auto now = boost::posix_time::microsec_clock::local_time();
                            std::ostringstream oss;
                            boost::posix_time::time_facet* facet = new boost::posix_time::time_facet("%Y-%m-%d %H:%M:%S.%f");
                            oss.imbue(std::locale(std::locale::classic(), facet));
                            oss << now;
                            os << "[" << oss.str() << "]" << " [" << log_type << "] "
                                << view.attribute_values()[message].extract<std::string>(); });

	boost::log::core::get()->add_sink(sink);

	// TODO: File save or send to Datadog
}