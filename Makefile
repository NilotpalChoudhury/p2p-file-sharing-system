.PHONY: all client tracker clean

all: client tracker

client:
	$(MAKE) -C client

tracker:
	$(MAKE) -C tracker

clean:
	$(MAKE) -C client clean
	$(MAKE) -C tracker clean
