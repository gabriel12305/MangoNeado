CC = gcc
CFLAGS = -Wall -g
LIBS = -lm

# Archivos fuente
SRCS = escaner.c robot.c
# Archivos objeto
OBJS = escaner.o robot.o
# Ejecutables
EXEC = escaner robot

all: $(EXEC)

escaner: escaner.o
	$(CC) -o $@ $^ $(LIBS)

robot: robot.o
	$(CC) -o $@ $^ -lpthread $(LIBS)

escaner.o: escaner.c datos.h
	$(CC) $(CFLAGS) -c $<

robot.o: robot.c datos.h
	$(CC) $(CFLAGS) -c $<

# Regla para limpiar archivos generados
clean:
	rm -f $(OBJS) $(EXEC)

.PHONY: clean

