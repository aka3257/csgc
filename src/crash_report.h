#pragma once

void InstallCrashReporter();

void LogStackScan(const char* tag);

const char* DescribeAddress(const void* addr);
