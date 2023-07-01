# syntax=docker/dockerfile:1

FROM ubuntu:latest
WORKDIR ./opt/topoviewer

# Download dist folder
# COPY ./dist /opt/bngblaster

USER root:root

# Install packages
RUN apt-get update && apt-get install -y openssh-server iproute2 iputils-ping vim wget

# Config SSH
RUN echo "HostKeyAlgorithms ssh-dss,ecdsa-sha2-nistp256,ssh-ed25519" >> /etc/ssh/ssh_config    
RUN echo "KexAlgorithms diffie-hellman-group1-sha1,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,diffie-hellman-group-exchange-sha256,diffie-hellman-group14-sha1" >> /etc/ssh/ssh_config    
RUN useradd -rm -d /home/ubuntu -s /bin/bash -g root -G sudo -u 1000 suuser 
RUN echo 'suuser:suuser' | chpasswd
RUN useradd -rm -d /home/ubuntu -s /bin/bash -g root -G sudo -u 1000 admin 
RUN echo 'admin:admin' | chpasswd
RUN service ssh start

# Get and install BngBlaster
RUN wget https://github.com/rtbrick/bngblaster/releases/download/0.8.22/bngblaster-0.8.22-ubuntu-22.04_amd64.deb
RUN apt-get install bngblaster-0.8.22-ubuntu-22.04_amd64.deb

# Create /opt/bngblaster
RUN mkdir /opt/bngblaster

#expose port 
EXPOSE 8080 22

ENTRYPOINT [ "/opt/bngblaster" ]
