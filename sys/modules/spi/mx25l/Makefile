# $FreeBSD$

.PATH:	${SRCTOP}/sys/dev/flash

KMOD=	mx25l
SRCS=	mx25l.c

# Generated files...
SRCS+=	\
	bus_if.h \
	device_if.h \
	spibus_if.c \
	spibus_if.h \

.if !empty(OPT_FDT)
SRCS+=	ofw_bus_if.h
.endif

.include <bsd.kmod.mk>
