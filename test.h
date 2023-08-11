#ifndef PERFTEST_H
#define PERFTEST_H

#include <string>

void perf_test_driver(const std::string &directory_path, int (*roundtrip)(const std::string&, std::ostream*, int*, int*, const int));

#endif