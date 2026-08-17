// http.go serves the dashboard: a static single-page UI plus a small JSON
// API. Like socket.go, every handler is a thin client -- it builds a
// cmdRequest and sends it on d.cmds, then writes back whatever the main
// loop goroutine replies. Handlers never touch Daemon fields directly, so
// the main loop stays the only goroutine that reads/writes panel state.
package daemon

import (
	"context"
	"embed"
	"encoding/json"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"strconv"
	"time"
)

//go:embed static/index.html
var staticFS embed.FS

// dashboardRequestTimeout mirrors socket.go's write deadline: the main loop
// can be mid blocking animation (several seconds) when a request arrives,
// so this needs real headroom rather than a tight API-style timeout.
const dashboardRequestTimeout = 30 * time.Second

// ServeHTTP starts the dashboard's HTTP server and blocks until ctx is
// cancelled, at which point it shuts the server down. Callers run this in
// its own goroutine alongside Run.
func (d *Daemon) ServeHTTP(ctx context.Context, addr string) error {
	sub, err := fs.Sub(staticFS, "static")
	if err != nil {
		return err
	}
	mux := http.NewServeMux()
	mux.Handle("/", http.FileServer(http.FS(sub)))
	mux.HandleFunc("/api/status", d.handleStatus)
	mux.HandleFunc("/api/pomodoro", d.handlePomodoro)
	mux.HandleFunc("/api/cancel", d.handleSimple(verbCancel))
	mux.HandleFunc("/api/off", d.handleSimple(verbOff))
	mux.HandleFunc("/api/normal", d.handleSimple(verbNormal))
	mux.HandleFunc("/api/brightness", d.handleBrightness)
	mux.HandleFunc("/api/trigger", d.handleTrigger)

	srv := &http.Server{
		Addr:              addr,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
		WriteTimeout:      dashboardRequestTimeout,
	}
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		srv.Shutdown(shutdownCtx)
	}()

	log.Printf("dashboard listening on http://%s", addr)
	err = srv.ListenAndServe()
	if err == http.ErrServerClosed {
		return nil
	}
	return err
}

// dispatch sends req on d.cmds and waits for the main loop's reply, same as
// a socket client would.
func (d *Daemon) dispatch(req cmdRequest) string {
	reply := make(chan string, 1)
	req.reply = reply
	d.cmds <- req
	return <-reply
}

func (d *Daemon) handleStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	snapshot := d.dispatch(cmdRequest{verb: verbSnapshot})
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprint(w, snapshot)
}

func (d *Daemon) handleSimple(verb cmdVerb) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		writeResult(w, d.dispatch(cmdRequest{verb: verb}))
	}
}

func (d *Daemon) handlePomodoro(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var body struct {
		Duration string `json:"duration"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		http.Error(w, "bad request: "+err.Error(), http.StatusBadRequest)
		return
	}
	dur, err := time.ParseDuration(body.Duration)
	if err != nil {
		http.Error(w, "bad duration: "+err.Error(), http.StatusBadRequest)
		return
	}
	writeResult(w, d.dispatch(cmdRequest{verb: verbPomodoro, dur: dur}))
}

func (d *Daemon) handleBrightness(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var body struct {
		Target string `json:"target"`
		Value  int    `json:"value"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		http.Error(w, "bad request: "+err.Error(), http.StatusBadRequest)
		return
	}
	if body.Value < 0 || body.Value > 255 {
		http.Error(w, "value out of range 0-255: "+strconv.Itoa(body.Value), http.StatusBadRequest)
		return
	}
	writeResult(w, d.dispatch(cmdRequest{verb: verbBrightness, target: body.Target, value: uint8(body.Value)}))
}

func (d *Daemon) handleTrigger(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var body struct {
		Name string `json:"name"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		http.Error(w, "bad request: "+err.Error(), http.StatusBadRequest)
		return
	}
	writeResult(w, d.dispatch(cmdRequest{verb: verbTrigger, trigger: body.Name}))
}

func writeResult(w http.ResponseWriter, result string) {
	w.Header().Set("Content-Type", "application/json")
	if len(result) >= 6 && result[:6] == "error:" {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(map[string]string{"error": result[7:]})
		return
	}
	json.NewEncoder(w).Encode(map[string]string{"result": result})
}
