#include "FilterHandler.h"
#include "Logger.h"

FilterHandler::FilterHandler(FilterConfig cfg)
    : filter_config(std::move(cfg)) {}

FilterHandler::~FilterHandler() = default;

std::pair<AVFilterContext*, AVFilterContext*> FilterHandler::get_filter_context() {
    // Logger::get_instance().print_log(AV_LOG_INFO, "FilterHandler(no-op): returning nullptr contexts");
    return {nullptr, nullptr};
}
