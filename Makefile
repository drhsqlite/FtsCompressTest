.include <bsd.own.mk>

PROGS=	testfts
SRCS.testfts=	testfts.c sqlite3.c sqlite3.h
MKMAN=no

CPPFLAGS+=-DSQLITE_ENABLE_FTS3
CPPFLAGS+=-DSQLITE_ENABLE_FTS3_PARENTHESIS
LDADD+=	-lz

.include <bsd.prog.mk>
