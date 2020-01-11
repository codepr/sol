FROM rikorose/gcc-cmake
COPY . /sol
WORKDIR /sol
EXPOSE 1883
RUN cmake -DDEBUG=0 . && make
CMD ./sol -a 0.0.0.0
