# Fuzz Face ngspice renders. Drop a DI in with: make render INPUT=di.wav
INPUT ?=

render:
	python3 render.py $(if $(INPUT),--input $(INPUT))

# e.g. make custom SET="vcc=5 bf1=40 temp=60" NAME=my_wreck
custom:
	python3 render.py $(if $(INPUT),--input $(INPUT)) $(foreach s,$(SET),--set $(s)) --name $(NAME)

clean:
	rm -rf renders

.PHONY: render custom clean
