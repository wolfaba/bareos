Build Bareos Packages
=====================

Creating Bareos Packages from the Cloned Sourcecode
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cloning and initial steps
^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: shell-session

    sudo apt-get install git dpkg-dev devscripts fakeroot
    git clone https://github.com/bareos/bareos
    cd bareos/core
    dpkg-checkbuilddeps

You then need to install all packages that :command:`dpkg-checkbuilddeps` lists as
required

Changelog preparation
'''''''''''''''''''''

To build a Debian package, you will need a :file:`debian/changelog` file.
The version of the package will be taken from the upmost entry in this file.

In the git checkout, the file :file:`debian/changelog` does not exist,
as when the Bareos project builds packages, the changelog file will created and updated automatically, based on the file file:`platforms/packaging/bareos.changes` and the current Bareos version defined in :file:`src/include/version.h`.

On manual building, these steps has to be done manually:

.. code-block:: shell-session

    cp -a platforms/packaging/bareos.changes debian/changelog
    VERSION=$(sed -n -r 's/#define VERSION "(.*)"/\1/p'  src/include/version.h)
    dch -v $VERSION "Switch version number"


Building the Debian Packages
''''''''''''''''''''''''''''

The following command builds Bareos source and creates the Bareos packages.
They will be stored in the parent dictory (:file:`..`).

.. code-block:: shell-session

    fakeroot debian/rules binary

Compiling and installing Bareos locally
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

While :ref:`DeveloperGuide/generaldevel:Building the test-environment`
describes the minimal settings to compile Bareos
(and make it runable inside the build directory),
this section shows typlical settings
and prepare Bareos also for installation using :command:`make install`.

Disclaimer: This process makes use of development-oriented compiler
flags. If you want to compile Bareos to be similar to a Bareos compiled
with production intent, please refer to section :ref:`DeveloperGuide/generaldevel:Using the same flags as in production`.

Cloning and initial steps
^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: shell-session

    git clone https://github.com/bareos/bareos
    cd bareos/core

Compiling and locally installing Bareos
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: shell

    #!/bin/bash
    export CFLAGS="-g -Wall"
    export CXXFLAGS="-g -Wall"

    # specifies the directory in which bareos will be installed
    DESTDIR=~/bareos

    mkdir $DESTDIR

    CMAKE_BUILDDIR=cmake-build

    mkdir ${CMAKE_BUILDDIR}
    pushd ${CMAKE_BUILDDIR}

    # In a normal installation, Dbaseport=9101 is used.
    # However, for testing purposes, we make use of port 8001.
    cmake  .. \
      -DCMAKE_VERBOSE_MAKEFILE=ON \
      -DBUILD_SHARED_LIBS:BOOL=ON \
      -Dbaseport=8001 \
      -DCMAKE_INSTALL_PREFIX:PATH=$DESTDIR \
      -Dprefix=$DESTDIR \
      -Dworkingdir=$DESTDIR/var/ \
      -Dpiddir=$DESTDIR/var/ \
      -Dconfigtemplatedir=$DESTDIR/lib/defaultconfigs \
      -Dsbin-perm=755 \
      -Dpython=yes \
      -Dreadline=yes \
      -Dbatch-insert=yes \
      -Ddynamic-cats-backends=yes \
      -Ddynamic-storage-backends=yes \
      -Dscsi-crypto=yes \
      -Dlmdb=yes \
      -Dndmp=yes \
      -Dipv6=yes \
      -Dacl=yes \
      -Dxattr=yes \
      -Dpostgresql=yes \
      -Dmysql=yes \
      -Dsqlite3=yes \
      -Dtcp-wrappers=yes \
      -Dopenssl=yes \
      -Dincludes=yes

    popd

You will now have to do the following:

.. code-block:: shell

    # This path corresponds to the $CMAKE_BUILDDIR variable. If you used a directory other than the default ```cmake-build```, you will have to alter the path accordingly.
    cd cmake-build
    make
    make install

Configuring Bareos
^^^^^^^^^^^^^^^^^^

Before you can successfully use your local installation, it requires
additional configuration.

.. code-block:: shell

    # You have to move to the local installation directory.
    # This path corresponds to the $DESTDIR variable.
    # If you used a directory other than the default ```~/baroes```,
    # you will have to alter the path accordingly.
    cd ~/bareos

    # Copy configuration files, only neccesary on initial install
    cp -a lib/defaultconfigs/* etc/bareos/

    # Replace template string XXX_REPLACE_WITH_DATABASE_DRIVER_XXX
    # with the database you want to use.
    sed -i 's/XXX_REPLACE_WITH_DATABASE_DRIVER_XXX/sqlite3/' etc/bareos/bareos-dir.d/catalog/MyCatalog.conf



.. code-block:: shell

    # sets up server
    # creates bareos database (requires sqlite3 package in case of sqlite3 installation)
    lib/bareos/scripts/create_bareos_database
    lib/bareos/scripts/make_bareos_tables
    lib/bareos/scripts/grant_bareos_privileges

Launching your local Bareos installation
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: shell

    # launches director in debug mode in foreground
    sbin/bareos-dir -f -d100

    # displays status of bareos daemons
    lib/bareos/scripts/bareos status

    # The start command launches all daemon (director, storage-daemon and file-daemon),
    # if not already launched.
    # We launched the director seperately for debugging purposes.
    lib/bareos/scripts/bareos start

    # displays status of bareos daemons
    lib/bareos/scripts/bareos status

    # launches bconsole to connect to director
    bin/bconsole
