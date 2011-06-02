TARGET	:= gx
OBJS	:= main.o request.o response.o ls.o mime.o
SRCS 	:= main.c request.c response.c ls.c mime.c
CC 		:= gcc
CFLAGS 	:= -march=native -O2 -pipe -fomit-frame-pointer -Wall
LDLIBS	+= -lpthread

all: $(OBJS)
	$(CC) $(CFLAGS) $(LDLIBS) $(OBJS) -o $(TARGET)
	strip $(TARGET)

main.o:main.c
	$(CC) $(CFLAGS) -c $^ -o $@
request.o:request.c
	$(CC) $(CFLAGS) -c $^ -o $@
response.o:response.c
	$(CC) $(CFLAGS) -c $^ -o $@
ls.o:ls.c
	$(CC) $(CFLAGS) -c $^ -o $@
mime.o:mime.c
	$(CC) $(CFLAGS) -c $^ -o $@
	
clean:
	rm -f *~  *.o $(TARGET)

.PHONY :clean
