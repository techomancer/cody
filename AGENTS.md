This project is a DOS/IRIX/linux agent for local LLMs

It uses MIPSPro 7.4.4 on IRIX 
It uses Open WATCOM C 1.9 for DOS builds
Since mTCP is using C++ we are going to roll with it and use C++, but nothing to crazy, think C with objects.
We prefer static memory, no dynamic allocations if possible or allocate only once and reuse.
We have to fit in DOS.

The architecture

- main loop with polling on unix/linux and pushing mtcp in DOS
- abstracted input using console and termcap/terminfo on unix and int 16h and raw screen read/write in DOS
- simple unencrypted HTTP requests to LLM like ollama or LMstudio running on local network
- enough json to build a request and parse the response
- MCP defined as command, description and function (with context pointer) to execute the command. variety of functions for file io, directory/file manipulation and so on specific to host os
- possibly multiple parallel sessions, hence context should be kept per session. but we start with one
- abstract TCP since mTCP and unix TCP are quite different. Socket class with two implementations.

C++ dialect
-----------

Target C++98 throughout. All three compilers (Open Watcom 1.9, MIPSPro 7.4.4,
gcc 3.x/4.x) support it. Do not use C++11 or later features.

Allowed: bool, inline, static class methods, simple templates if needed.
Avoid:   nullptr (use 0 or NULL), auto, range-for, <cstdint> (use types.h),
         initializer lists, lambdas, move semantics, std::string, STL.

Directory layout
----------------

  src/         Shared platform-neutral code (compiled on all targets)
  UNIX/        Code shared by both Linux and IRIX (POSIX)
  LINUX/       Linux-specific files and GNU Makefile
  IRIX/        IRIX-specific files and MIPSPro smake Makefile
  DOS/         DOS-specific files, Open Watcom Makefile, mTCP library
  DOS/mTCP/    Michael Brutman's mTCP TCP/IP stack (reference + library)

Platform split rule
-------------------

Prefer separate per-platform files over ifdefs. The compiler macros
__WATCOMC__ (Open Watcom), __sgi (MIPSPro/IRIX), and __GNUC__ (gcc/g++)
are used only for small one-liner differences inside shared files; anything
larger than a single expression gets its own platform file.

Build summary
-------------

  Linux : g++ -std=c++98, sources from src/ + UNIX/, Makefile in LINUX/
  IRIX  : MIPSPro CC -n32, sources from src/ + UNIX/, Makefile in IRIX/
  DOS   : Open Watcom wpp -ml, sources from src/ + DOS/, Makefile in DOS/
          mTCP headers: DOS/mTCP/TCPINC/
          mTCP library source: DOS/mTCP/TCPLIB/

LLM protocol
------------

HTTP POST to /v1/chat/completions (OpenAI-compatible API).
Works with Ollama, LMStudio, llama.cpp server, vLLM, and OpenAI.
Streaming uses SSE format: each line is "data: {...}" or "data: [DONE]".
Strip the "data: " prefix, skip "[DONE]", parse the JSON chunk.
Tool calls use the OpenAI delta.tool_calls streaming format.
