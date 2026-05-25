CC = C:\msys64\ucrt64\bin\gcc.exe
CFLAGS = -std=c11 -O2 -Wall -Wextra -Iinclude
LDFLAGS = -lcurl
SRC = src/main.c src/cf_api.c src/cJSON.c src/json_utils.c \
      src/analyzer.c src/htmlgen.c src/rate_limiter.c
OBJ = $(SRC:.c=.o)
OUT = bin/cf_crawler.exe

all: $(OUT)

$(OUT): $(OBJ) | bin tmp
	$(CC) $(OBJ) -o $(OUT) $(LDFLAGS)

bin:
	if not exist bin mkdir bin

tmp:
	if not exist tmp mkdir tmp

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	-del /Q $(OBJ) $(OUT) 2>nul
	-rmdir /S /Q tmp data output 2>nul

run: $(OUT)
	$(OUT) sample_users.txt

.PHONY: all clean run
