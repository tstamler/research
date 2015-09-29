all:
#	gcc -I../include/ -D__x86__ -O2 -DNDEBUG -Wno-unused-function -Wall -Wextra ll_parsec.c parsec.c ck_epoch.c -o bench -lpthreadn
#	gcc -I../include/ -D__x86__ -O2 -Wno-unused-function -Wall -Wextra ll_parsec.c parsec.c ck_epoch.c -o bench -lpthread -lurcu
#	gcc -I../include/ -DRCU_MB -D__x86__ -O2 -DNDEBUG -Wno-unused-function -Wall -Wextra ll_parsec.c parsec.c ck_epoch.c -o bench -lurcu-mb -lpthread
	gcc -I../include/ -DRCU_MB -D__x86__ -O2 -Wno-unused-function -Wall -Wextra ll_parsec.c parsec.c ck_epoch.c -o bench -lurcu-mb -lpthread
#	gcc -I../include/ -DRCU_SIGNAL -D__x86__ -O2 -Wno-unused-function -Wall -Wextra ll_parsec.c parsec.c ck_epoch.c -o bench -lurcu-signal -lpthread
