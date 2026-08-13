package daemon

type Mode int

const (
	ModeRegular Mode = iota
	ModeMusic
	ModePomodoro
	ModeOff
)

func (m Mode) String() string {
	switch m {
	case ModeRegular:
		return "regular"
	case ModeMusic:
		return "music"
	case ModePomodoro:
		return "pomodoro"
	case ModeOff:
		return "off"
	default:
		return "unknown"
	}
}
