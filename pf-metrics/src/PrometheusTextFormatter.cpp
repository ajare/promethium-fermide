#include "metrics/PrometheusTextFormatter.h"
#include <cmath>
#include <charconv>
#include <set>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
namespace metrics {
namespace {
std::string escape(std::string const& text, bool label) {
    std::string result;
    for (char c : text) {
        if (c == '\\') result += "\\\\";
        else if (c == '\n') result += "\\n";
        else if (c == '"' && label) result += "\\\"";
        else result += c;
    }
    return result;
}
std::string number(double n) {
    if (std::isnan(n)) return "NaN";
    if (std::isinf(n)) return n < 0 ? "-Inf" : "+Inf";
    char buffer[64];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), n);
    return std::string(buffer, result.ptr);
}
}
std::string PrometheusTextFormatter::format(core::MetricsRegistry const& registry) {
    std::ostringstream out; out.imbue(std::locale::classic());
    auto emit = [&](std::string const& name, core::MetricLabels const& labels, std::string const& value) {
        out << name;
        if (!labels.empty()) {
            out << '{'; bool first = true;
            for (auto const& [key, text] : labels) { if (!first) out << ','; first = false; out << key << "=\"" << escape(text, true) << '"'; }
            out << '}';
        }
        out << ' ' << value << '\n';
    };
    std::set<std::string> names;
    for (auto const& [key, family] : registry.families) {
        auto name = key;
        if (family.type == core::MetricType::Counter && !name.ends_with("_total")) name += "_total";
        if (!names.insert(name).second) throw std::invalid_argument("Duplicate metric family");
        if (family.type == core::MetricType::Histogram) for (auto suffix : {"_bucket", "_sum", "_count"}) {
            if (!names.insert(name + suffix).second || registry.families.count(name + suffix)) throw std::invalid_argument("Histogram family collision");
        }
        out << "# HELP " << name << ' ' << escape(family.help, false) << '\n';
        out << "# TYPE " << name << ' ' << (family.type == core::MetricType::Counter ? "counter" : family.type == core::MetricType::Gauge ? "gauge" : "histogram") << '\n';
        for (auto const& [labels, sample] : family.samples) {
            if (sample.labels != labels) throw std::invalid_argument("Inconsistent metric labels");
            if (family.type != core::MetricType::Histogram) emit(name, labels, number(sample.value));
            else {
                if (labels.count("le")) throw std::invalid_argument("Reserved histogram label: le");
                auto bucket = labels;
                for (auto const& [bound, count] : sample.buckets) { bucket["le"] = number(bound); emit(name + "_bucket", bucket, std::to_string(count)); }
                bucket["le"] = "+Inf"; emit(name + "_bucket", bucket, std::to_string(sample.count));
                emit(name + "_sum", labels, number(sample.value)); emit(name + "_count", labels, std::to_string(sample.count));
            }
        }
    }
    return out.str();
}
}
