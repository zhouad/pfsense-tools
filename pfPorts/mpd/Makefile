# New ports collection makefile for:	mpd
# Date created:				25 Feb 1999
# Whom:					Archie Cobbs
#
# $FreeBSD: ports/net/mpd/Makefile,v 1.51 2007/02/12 07:57:25 sem Exp $
#

PORTNAME=	mpd
PORTVERSION=	3.18
PORTREVISION=	5
CATEGORIES=	net
MASTER_SITES=	${MASTER_SITE_LOCAL}
MASTER_SITE_SUBDIR=	archie
DIST_SUBDIR=	mpd
NO_STAGE=	yes

MAINTAINER=	ports@FreeBSD.org
COMMENT=	Multi-link PPP daemon based on netgraph(4) [legacy version]

# lang/mpd
CONFLICTS=	mpd-[^3]*

MTREE_FILE=	${FILESDIR}/mpd.mtree
MAN8=		mpd.8
MANCOMPRESSED=	maybe

USE_OPENSSL=	yes
MAKE_ENV=	COPTS="${COPTS} -I${OPENSSLINC}" LDADD="${LDADD} -L${OPENSSLLIB}"

USE_RC_SUBR=	mpd

pre-build:
	@${ECHO}
	@${ECHO} "Use WITH_OPENSSL_PORT=yes if you use OPENSSL from ports"
	@${ECHO}

.include <bsd.port.mk>
