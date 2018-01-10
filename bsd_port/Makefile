VALID_CATEGORIES+= igports
PORTNAME=	testtool
PORTVERSION=	_PV_
CATEGORIES=	igports

MAINTAINER=	kajetan.staszkiewicz@innogames.de
COMMENT=	Testtool checks health of loadbalanced services.

USES=		gmake

LIB_DEPENDS=	libevent-2.1.so.6:devel/libevent \
		libpq.so.5:databases/postgresql93-client \
		libyaml-cpp.so.0.5.3:devel/yaml-cpp \
		libboost_system.so.1.64.0:devel/boost-libs

BUILD_DEPENDS=	libfmt>=3.0.1_1:devel/libfmt

PLIST_FILES=	/usr/local/sbin/testtool \
		/usr/local/etc/rc.d/testtool

.include <bsd.port.options.mk>
.include <bsd.port.mk>

