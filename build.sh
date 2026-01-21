mkdir build
cd build
cmake .. \
	-DCMAKE_EXPORT_COMPILE_COMMANDS=1 \
	-DCMAKE_C_COMPILER=/home/xiaoxuanx/bin/gcc \
	-DCMAKE_CXX_COMPILER=/home/xiaoxuanx/bin/g++ \
	-DUSE_AIO=ON
make -j
