package main

import (
	"io"
	"io/ioutil"
	"log"
	"os"
	"os/exec"
	"strings"
)

func RunAndSave(output string, name string, arg ...string) {
	cmd := exec.Command(name, arg...)

	f, err := os.Create(output)
	if err != nil {
		log.Fatal(err)
	}

	cmd.Stdout = f
	cmd.Stderr = f

	err = cmd.Start()
	if err != nil {
		log.Fatal(err)
	}
	cmd.Wait()
}

func MakeLogs(outDir string) {
	os.MkdirAll(outDir, 0700)
	RunAndSave(outDir+"/lspci.log", "lspci", "-nnvvvxxxx")
	RunAndSave(outDir+"/dmidecode.log", "dmidecode")
	RunAndSave(outDir+"/acpidump.log", "acpidump")
	/* FIXME:XX */
	RunAndSave(outDir+"/inteltool.log", "../inteltool/inteltool", "-a")
	RunAndSave(outDir+"/ectool.log", "../ectool/ectool", "-a")

	SysDir := "/sys/class/sound/card0/"
	files, _ := ioutil.ReadDir(SysDir)
	for _, f := range files {
		if (strings.HasPrefix(f.Name(), "hw") || strings.HasPrefix(f.Name(), "hdaudio")) && f.IsDir() {
			in, err := os.Open(SysDir + f.Name() + "/init_pin_configs")
			defer in.Close()
			if err != nil {
				log.Fatal(err)
			}
			out, err := os.Create(outDir + "/pin_" + strings.Replace(f.Name(), "hdaudio", "hw", -1))
			if err != nil {
				log.Fatal(err)
			}
			defer out.Close()
			io.Copy(out, in)
		}
	}

	ProcDir := "/proc/asound/card0/"
	files, _ = ioutil.ReadDir(ProcDir)
	for _, f := range files {
		if strings.HasPrefix(f.Name(), "codec#") && !f.IsDir() {
			in, err := os.Open(ProcDir + f.Name())
			defer in.Close()
			if err != nil {
				log.Fatal(err)
			}
			out, err := os.Create(outDir + "/" + f.Name())
			if err != nil {
				log.Fatal(err)
			}
			defer out.Close()
			io.Copy(out, in)
		}
	}

	for _, fname := range []string{"cpuinfo", "ioports"} {
		in, err := os.Open("/proc/" + fname)
		defer in.Close()
		if err != nil {
			log.Fatal(err)
		}
		out, err := os.Create(outDir + "/" + fname + ".log")
		if err != nil {
			log.Fatal(err)
		}
		defer out.Close()
		io.Copy(out, in)
	}
}