CROSS_COMPILE ?=
CXX := $(CROSS_COMPILE)g++

all:
	$(CXX) -g -O2 main.cpp -o c2cap -L libvphevcodec -l vphevcodec

