VALID_CATEGORIES+= igports
PORTNAME=	testtool
DISTVERSION=	_DV_
CATEGORIES=	igports

MAINTAINER=	kajetan.staszkiewicz@innogames.de
COMMENT=	Testtool checks health of loadbalanced services.

USES=		cmake

LIB_DEPENDS=	libyaml-cpp.so:devel/yaml-cpp \
		event.so:devel/libevent \
		event_core.so:devel/libevent \
		event_pthreads.so:devel/libevent \
		event_openssl.so:devel/libevent

BUILD_DEPENDS=	${NONEXISTENT}:devel/libevent \
		${NONEXISTENT}:devel/boost-libs \
		${NONEXISTENT}:databases/postgresql96-client \
		${NONEXISTENT}:devel/nlohmann-json \
		${NONEXISTENT}:devel/googletest \
		${NONEXISTENT}:devel/libfmt

PLIST_FILES=	/usr/local/sbin/testtool \
		/usr/local/etc/rc.d/testtool

.include <bsd.port.options.mk>
.include <bsd.port.mk>

