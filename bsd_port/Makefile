VALID_CATEGORIES+= igports
PORTNAME=	testtool
PORTVERSION=	_PV_
CATEGORIES=	igports

MAINTAINER=	kajetan.staszkiewicz@innogames.de
COMMENT=	Testtool checks health of loadbalanced services.

USES=		gmake

LIB_DEPENDS=	libevent-2.1.so.6:devel/libevent \
		libyaml-cpp.so:devel/yaml-cpp \

BUILD_DEPENDS=	libfmt>=0:devel/libfmt \
		libpq.so.5:databases/postgresql93-client \
		libboost_system.so:devel/boost-libs

PLIST_FILES=	/usr/local/sbin/testtool \
		/usr/local/etc/rc.d/testtool

.include <bsd.port.options.mk>
.include <bsd.port.mk>

