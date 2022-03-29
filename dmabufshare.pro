TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += \
	main.c

HEADERS += \
	render.h \
	socket.h \
	window.h

LIBS += -lEGL -lGL -lX11 -lm
