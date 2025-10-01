#!/bin/sh
set -e
echo $PATH
echo 'apt update'
apt-get update


if ! service --status-all | grep -Fq 'prometheus-node-exporter';
then
    echo 'Installing prometheus-node-exporter'
    apt-get install prometheus-node-exporter -y
    systemctl enable prometheus-node-exporter
    systemctl start prometheus-node-exporter
fi

if ! command -v pm2
then
	if ! command -v nvm
	then
		echo 'Installing nvm...'
		curl -o install_nvm.sh https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.2/install.sh
		chmod +x install_nvm.sh && ./install_nvm.sh
		export NVM_DIR="$HOME/.nvm"
		[ -s "$NVM_DIR/nvm.sh" ] && \. "$NVM_DIR/nvm.sh"
		[ -s "$NVM_DIR/bash_completion" ] && \. "$NVM_DIR/bash_completion"
		nvm install node		
		echo export PATH="'$NVM_BIN:$PATH'" >> ~/.bashrc;
		export PATH=$NVM_BIN:$PATH
		rm install_nvm.sh
	fi
	echo 'Installing pm2...'
	npm install --location=global pm2
	pm2 install pm2-logrotate
	pm2 set pm2-logrotate:max_size 100M
	pm2 set pm2-logrotate:compress true
fi

if ! command -v htop
then
	echo 'Installing htop...'
	apt-get install htop -y
fi

if ! command -v tmux
then
	echo 'Installing tmux...'
	apt-get install tmux -y
fi

if ! command -v pyenv
then
    echo 'Installing pyenv'
    sudo apt install -y make build-essential libssl-dev zlib1g-dev libbz2-dev libreadline-dev libsqlite3-dev wget curl llvm libncursesw5-dev xz-utils tk-dev libxml2-dev libxmlsec1-dev libffi-dev liblzma-dev
    curl https://pyenv.run | bash
	export PYENV_ROOT="$HOME/.pyenv"
	export PATH="$PYENV_ROOT/bin:$PATH"
	eval "$(pyenv init --path)"
	eval "$(pyenv init -)"
    echo 'export PYENV_ROOT="$HOME/.pyenv"\nexport PATH="$PYENV_ROOT/bin:$PATH"' >> ~/.bashrc
    echo 'eval "$(pyenv init --path)"\neval "$(pyenv init -)"' >> ~/.bashrc
fi
if ! pyenv versions | grep -Fq '3.10.9';
then
    echo 'Installing and activating Python 3.10.9'
    pyenv install 3.10.9
    pyenv global 3.10.9
fi

python -m pip install -U pyopenssl cryptography

echo "Installing taos"
python -m pip install -e .

cd simulate/trading
if [ ! -d "vcpkg" ]; then
	git clone https://github.com/microsoft/vcpkg.git
fi
cd vcpkg && git reset --hard e140b1fde236eb682b0d47f905e65008a191800f && cd ..
apt-get install -y curl zip unzip tar make pkg-config autoconf autoconf-archive libcurl4-openssl-dev
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

python -m pip install -e .

. /etc/lsb-release
echo "Ubuntu Version $DISTRIB_RELEASE"
if ! g++ -dumpversion | grep -q "14"; then
	echo "g++ is not at version 14!  Checking for g++-14.."
	if ! command -v g++-14
	then
		echo "g++-14 is not available.  Installing.."
		if [ "$DISTRIB_RELEASE" = "22.04" ]; then
				apt-get install software-properties-common libmpfr-dev libgmp3-dev libmpc-dev -y
				wget http://ftp.gnu.org/gnu/gcc/gcc-14.1.0/gcc-14.1.0.tar.gz
				tar -xf gcc-14.1.0.tar.gz
				cd gcc-14.1.0
				./configure -v --build=x86_64-linux-gnu --host=x86_64-linux-gnu --target=x86_64-linux-gnu --prefix=/usr/local/gcc-14.1.0 --enable-checking=release --enable-languages=c,c++ --disable-multilib --program-suffix=-14.1.0
				make
				make install
				cd ..
				rm -r gcc-14.1.0
				rm gcc-14.1.0.tar.gz
				update-alternatives --install /usr/bin/g++-14 g++-14 /usr/local/gcc-14.1.0/bin/g++-14.1.0 14
				export LD_LIBRARY_PATH="/usr/local/gcc-14.1.0/lib/../lib64:$LD_LIBRARY_PATH"
				echo 'export LD_LIBRARY_PATH="/usr/local/gcc-14.1.0/lib/../lib64:$LD_LIBRARY_PATH"' >> ~/.bashrc
		else
			apt-get -y install g++-14
		fi
	else
		echo "g++-14 is already available."
	fi
fi

if ! cmake --version | grep -q "3.29.7"; then
	echo "Installing cmake 3.29.7..."
	apt-get purge -y cmake
	wget https://github.com/Kitware/CMake/releases/download/v3.29.7/cmake-3.29.7.tar.gz
	tar zxvf cmake-3.29.7.tar.gz
	cd cmake-3.29.7
	./bootstrap
	make
	make install
	cd ..
	rm cmake-3.29.7.tar.gz
	rm -r cmake-3.29.7
else
	echo "cmake 3.29.7 is already installed."
fi

rm -r build || true
mkdir build
cd build
if ! g++ -dumpversion | grep -q "14"; then
	cmake -DCMAKE_BUILD_TYPE=Release -D CMAKE_CXX_COMPILER=g++-14 ..
else
	cmake -DCMAKE_BUILD_TYPE=Release ..
fi
cmake --build . -j "$(nproc)"