CC ?= $(CROSS_COMPILE)gcc


FLAGS = -Wall -Werror
TARGET = aesdsocket 
SRC = aesdsocket.c

all default: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(FLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET) *.o 
