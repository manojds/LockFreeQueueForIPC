cmake_minimum_required(VERSION 3.10)
project(lockfree_ipc_queue LANGUAGES CXX)



add_executable(lockfree_ipc_queue
	main.cpp
)

# Enable high optimization for low latency
target_compile_options(lockfree_ipc_queue PRIVATE -O3 -march=native -Wall -Wextra)
