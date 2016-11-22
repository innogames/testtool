VALID_CATEGORIES+= iglb
PORTNAME=	testtool
PORTVERSION= 	_PV_
CATEGORIES=	iglb

MAINTAINER=	kajetan.staszkiewicz@innogames.de
COMMENT=	Testtool checks health of loadbalanced services.
		
BUILD_DEPENDS=	postgresql93-client>=0:${PORTSDIR}/databases/postgresql93-client \
    		libevent2>=2.0.22_1:${PORTSDIR}/devel/libevent2

RUN_DEPENDS=	screen>=4.3.1_2:${PORTSDIR}/sysutils/screen

PLIST_FILES=	/usr/local/bin/testtool \
		/usr/local/bin/testtool_watchdog \
		/usr/local/etc/rc.d/testtool

# Override some targets
#checksum:
#do-fetch:
#do-extract:
#	cd ${WRKDIR} && git clone git@gitlab.innogames.de:sysadmins/testtool.git ${PORTNAME}-${PORTVERSION}
#	cd ${WRKDIR}/${PORTNAME}-${PORTVERSION} && git checkout tags/v${PORTVERSION}
#do-patch:
#do-configure:

.include <bsd.port.options.mk>
.include <bsd.port.mk>

