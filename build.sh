mkdir -p build
cd build
cmake .. \
	-DUSE_AIO=ON \
	-DCMAKE_EXPORT_COMPILE_COMMANDS=1 \
	-DCMAKE_C_COMPILER=/home/xiaoxuanx/bin/gcc \
	-DCMAKE_CXX_COMPILER=/home/xiaoxuanx/bin/g++
make -j
