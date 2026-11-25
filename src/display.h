#pragma once

#include "capture_service.h"

#include <fstream>
#include <string>

namespace netlab {

class Display {
public:
    explicit Display(const std::string& log_file);
    bool ready() const;
    const std::string& error() const;

    // 输出捕获日志与解析字段。
    void line(const std::string& message);
    void packet(const PacketRecord& record);

private:
    std::ofstream log_;
    std::string error_;
};

}
