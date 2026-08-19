#include "app/loading.h"

namespace lime {
namespace {
const std::string kNothing;
}

void LoadQueue::push(std::string label, Step run) {
    if (current_ >= steps_.size() && !steps_.empty()) clear();
    steps_.push_back({std::move(label), std::move(run)});
}

bool LoadQueue::step() {
    if (!active()) return false;

    detail_.clear();
    const float f = steps_[current_].run(detail_);
    fraction_ = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);

    if (f >= 1.0f) {
        ++current_;
        fraction_ = 0.0f;
        detail_.clear();
    }
    if (!active()) {
        clear();
        return false;
    }
    return true;
}

void LoadQueue::clear() {
    steps_.clear();
    current_ = 0;
    fraction_ = 0.0f;
    detail_.clear();
}

const std::string& LoadQueue::label() const {
    return active() ? steps_[current_].label : kNothing;
}

}
