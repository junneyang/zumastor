#!/bin/sh
#
# $Id$
#
# Set up the local machine (Debian or derivative) with the pass-through proxy.

set -e

# if apache2 was already installed and the default VirtualHost is already
# enabled, leave it alone.  Else install and disable the default VirtualHost
if [ ! -f /etc/apache2/sites-enabled/000-default ] ; then
  apt-get install -y apache2
  rm -f /etc/apache2/sites-enabled/000-default
else
  apt-get install -y apache2
fi

cp proxy /etc/apache2/sites-available/
ln -sf /etc/apache2/sites-available/proxy /etc/apache2/sites-enabled/
ln -sf /etc/apache2/mods-available/proxy.conf /etc/apache2/mods-enabled/
ln -sf /etc/apache2/mods-available/proxy.load /etc/apache2/mods-enabled/
ln -sf /etc/apache2/mods-available/cache.load /etc/apache2/mods-enabled/
ln -sf /etc/apache2/mods-available/disk_cache.load /etc/apache2/mods-enabled/

if [ ! -d /var/www/proxy ] ; then
  mkdir /var/www/proxy
  chown www-data:www-data /var/www/proxy
fi

/etc/init.d/apache2 restart
