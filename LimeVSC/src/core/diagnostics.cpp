#include "limecore.h"

namespace lime {

void Diagnostics::add(Diagnostic d) {
    if (d.severity == Severity::Error) ++errors_;
    else if (d.severity == Severity::Warning) ++warnings_;
    items_.push_back(std::move(d));
}

void Diagnostics::info(std::string msg, NodeId n) {
    add({Severity::Info, std::move(msg), {}, n, 0});
}
void Diagnostics::warn(std::string msg, NodeId n) {
    add({Severity::Warning, std::move(msg), {}, n, 0});
}
void Diagnostics::error(std::string msg, NodeId n) {
    add({Severity::Error, std::move(msg), {}, n, 0});
}

void Diagnostics::clear() {
    items_.clear();
    errors_ = 0;
    warnings_ = 0;
}

}
