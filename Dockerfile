FROM rikorose/gcc-cmake
RUN apt-get update && apt-get install -y uuid-dev
COPY . /sol
WORKDIR /sol
EXPOSE 1883
RUN cmake . && make
CMD ./sol -a 0.0.0.0
