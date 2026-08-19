#pragma once

#include <functional>
#include <string>
#include <vector>

namespace lime {

class LoadQueue {
public:
    using Step = std::function<float(std::string& detail)>;

    void push(std::string label, Step run);
    bool step();
    void clear();

    bool               active() const { return current_ < steps_.size(); }
    const std::string& label() const;
    const std::string& detail() const { return detail_; }
    float              fraction() const { return fraction_; }
    std::size_t index() const { return current_; }
    std::size_t count() const { return steps_.size(); }

private:
    struct Entry {
        std::string label;
        Step        run;
    };
    std::vector<Entry> steps_;
    std::size_t        current_ = 0;
    float              fraction_ = 0.0f;
    std::string        detail_;
};

}
