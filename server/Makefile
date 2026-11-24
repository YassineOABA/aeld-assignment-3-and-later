CC ?= $(CROSS_COMPILE)gcc

CFLAGS ?= -Wall -Werror
LDFLAGS ?=

TARGET = aesdsocket 
SRC = aesdsocket.c

all default: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET) *.o 
