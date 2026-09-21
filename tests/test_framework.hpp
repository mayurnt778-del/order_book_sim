#pragma once

#include <iostream>
#include <string>

class TestSuite {
public:
    void expect(bool condition, const std::string& message) {
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    int failures() const { return failures_; }

private:
    int failures_{0};
};

#define EXPECT_TRUE(suite, expression) (suite).expect((expression), #expression)
#define EXPECT_EQ(suite, actual, expected) \
    (suite).expect(((actual) == (expected)), std::string(#actual " == " #expected))
