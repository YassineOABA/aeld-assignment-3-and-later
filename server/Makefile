CC = $(CROSS_COMPILE)gcc


FLAGS = -g -O0 -Wall -Werror
TARGET = aesdsocket 
SRC = aesdsocket.c

all default: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(FLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET) *.o 
