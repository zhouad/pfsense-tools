diff -ur a/ipsec-tools-0.7.1/src/racoon/localconf.c b/ipsec-tools-0.7.1/src/racoon/localconf.c
--- src/racoon/localconf.c	2006-09-09 11:22:09.000000000 -0500
+++ src/racoon/localconf.c	2010-08-06 16:35:18.000000000 -0500
@@ -211,7 +211,8 @@
 		if (*p == '\0')
 			continue;	/* no 2nd parameter */
 		p--;
-		if (strncmp(buf, str, len) == 0 && buf[len] == '\0') {
+		if (strncmp(buf, "*", 2) == 0 ||
+		    (strncmp(buf, str, len) == 0 && buf[len] == '\0')) {
 			p++;
 			keylen = 0;
 			for (q = p; *q != '\0' && *q != '\n'; q++)

