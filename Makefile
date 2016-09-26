CROSS_COMPILE ?=
CXX := $(CROSS_COMPILE)g++

TARGET=amlcap
LIB=libvphevcodec
LDFLAGS += -lrt
LDFLAGS += -L $(LIB) -l vphevcodec

$(TARGET):
	$(MAKE) -C $(LIB)
	$(CXX) -g -O2 main.cpp -o $@ $(LDFLAGS)

clean:
	-rm -f $(TARGET)
	$(MAKE) -C $(LIB) clean
