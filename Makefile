all:
	clang++ -static -fuse-ld=lld -g -O2 -o fex_shm_stats_read fex_shm_stats_read.cpp -std=c++20 `pkgconf --libs --static ncursesw`
