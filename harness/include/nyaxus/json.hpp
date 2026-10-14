#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nyaxus {
namespace json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value {
   public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(Type type) : type_(type) {}
    Value(bool value);
    Value(int value);
    Value(long value);
    Value(long long value);
    Value(double value);
    Value(const char* value);
    Value(std::string value);
    Value(Array value);
    Value(Object value);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }

    bool as_bool(bool fallback = false) const;
    double as_number(double fallback = 0.0) const;
    long long as_integer(long long fallback = 0) const;
    const std::string& as_string() const;
    std::string as_string(const std::string& fallback) const;
    const Array& as_array() const;
    const Object& as_object() const;
    Array& mut_array();
    Object& mut_object();

    bool has(const std::string& key) const;
    const Value& at(const std::string& key) const;
    Value& at(const std::string& key);
    const Value& operator[](const std::string& key) const;
    Value& operator[](const std::string& key);
    bool contains(const std::string& key) const;

   private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::shared_ptr<Array> array_;
    std::shared_ptr<Object> object_;
};

Value parse(const std::string& text);
std::string stringify(const Value& value, bool pretty = false);

}  // namespace json
}  // namespace nyaxus