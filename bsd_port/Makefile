VALID_CATEGORIES+= iglb
PORTNAME=	testtool
PORTVERSION=	_PV_
CATEGORIES=	iglb

MAINTAINER=	kajetan.staszkiewicz@innogames.de
COMMENT=	Testtool checks health of loadbalanced services.

USES=		gmake

BUILD_DEPENDS=	libevent>=libevent-2.1.8:devel/libevent \
		postgresql93-client:databases/postgresql93-client \
		yaml-cpp>=0.5.3_2:devel/yaml-cpp:

BUILD_DEPENDS=	${ALL_DEPENDS}

RUN_DEPENDS=	${ALL_DEPENDS} \
		screen>=4.3.1_2:${PORTSDIR}/sysutils/screen

PLIST_FILES=	/usr/local/sbin/testtool \
		/usr/local/sbin/testtool_watchdog \
		/usr/local/etc/rc.d/testtool

.include <bsd.port.options.mk>
.include <bsd.port.mk>

