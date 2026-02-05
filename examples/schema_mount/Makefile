all: schema_mount_example

schema_mount_example: schema_mount_example.c
	gcc -Wall -g -o $@ $^ -lyang

clean:
	rm -f schema_mount_example
