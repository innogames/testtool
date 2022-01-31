VALID_CATEGORIES+= igports
PORTNAME=	testtool
DISTVERSION=	_DV_
CATEGORIES=	igports

MAINTAINER=	kajetan.staszkiewicz@innogames.de
COMMENT=	Testtool checks health of loadbalanced services.

USES=		cmake

LIB_DEPENDS=\
		libevent.so:devel/libevent \
		libintl.so:devel/gettext-runtime \
		libpq.so:databases/postgresql96-client \
		libyaml-cpp.so:devel/yaml-cpp

BUILD_DEPENDS=\
		${NONEXISTENT}:databases/postgresql96-client \
		${NONEXISTENT}:devel/boost-libs \
		${NONEXISTENT}:devel/gettext-runtime \
		${NONEXISTENT}:devel/googletest \
		${NONEXISTENT}:devel/libevent \
		${NONEXISTENT}:devel/libfmt \
		${NONEXISTENT}:devel/nlohmann-json \
		${NONEXISTENT}:devel/yaml-cpp

PLIST_FILES=	/usr/local/sbin/testtool \
		/usr/local/etc/rc.d/testtool

.include <bsd.port.options.mk>
.include <bsd.port.mk>

