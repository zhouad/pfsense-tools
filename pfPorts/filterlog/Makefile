PORTNAME=	filterlog
PORTVERSION=	0.1
PORTREVISION?=	0
CATEGORIES?=	sysutils
MASTER_SITES=	# empty
DISTFILES=	# none
EXTRACT_ONLY=	# empty
NO_STAGE=	yes

MAINTAINER?=	eri@pfsense.org
COMMENT?=	filterlog

WHERE=		sbin
PLIST_FILES=    ${WHERE}/${PORTNAME}

do-clean:
	cd files && make clean
	rm -rf work
do-extract:
	mkdir -p ${WRKSRC}

do-build:
	cd files && make

do-install:
	${INSTALL_PROGRAM} files/${PORTNAME} ${PREFIX}/${WHERE}/

.include <bsd.port.mk>
