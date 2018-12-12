VALID_CATEGORIES+= igports
PORTNAME=	testtool
PORTVERSION=	_PV_
CATEGORIES=	igports

MAINTAINER=	kajetan.staszkiewicz@innogames.de
COMMENT=	Testtool checks health of loadbalanced services.

USES=		cmake

LIB_DEPENDS=	libfmt.so:devel/libfmt \
		libyaml-cpp.so:devel/yaml-cpp

PLIST_FILES=	/usr/local/sbin/testtool \
		/usr/local/etc/rc.d/testtool

.include <bsd.port.options.mk>
.include <bsd.port.mk>

