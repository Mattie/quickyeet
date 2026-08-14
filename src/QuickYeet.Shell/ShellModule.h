#pragma once

#include <windows.h>

#include <atomic>

// {3DF187D7-6A2E-4E81-87C4-6B0D28A49678}
inline constexpr CLSID CLSID_QuickYeet = {
    0x3df187d7, 0x6a2e, 0x4e81, {0x87, 0xc4, 0x6b, 0x0d, 0x28, 0xa4, 0x96, 0x78}};

// {8D5E4991-C581-45A6-88C7-4611C27821F1}
inline constexpr CLSID CLSID_QuickYeetModern = {
    0x8d5e4991, 0xc581, 0x45a6, {0x88, 0xc7, 0x46, 0x11, 0xc2, 0x78, 0x21, 0xf1}};

extern HINSTANCE g_module_instance;
extern std::atomic<long> g_object_count;
extern std::atomic<long> g_server_locks;
