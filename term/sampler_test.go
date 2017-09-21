package term_test

import (
	"testing"

	"github.com/ambientsound/pms/term"
	termbox "github.com/nsf/termbox-go"
	"github.com/stretchr/testify/assert"
)

type sampleTest struct {
	name   string
	input  []termbox.Event
	output []term.Event
}

var samplerTests = []sampleTest{
	{
		"Resize event",
		[]termbox.Event{
			{Type: termbox.EventResize},
		},
		[]term.Event{
			{Type: term.EventResize},
		},
	},
	{
		"Keypress event on 'a'",
		[]termbox.Event{
			{
				Type: termbox.EventKey,
				Ch:   'a',
			},
		},
		[]term.Event{
			{
				Type: term.EventKey,
				Key:  term.KeyPress{0, 'a', 0},
			},
		},
	},
}

func TestSampler(t *testing.T) {
	for n, test := range samplerTests {
		sampler := term.New()

		t.Logf("### Test %d: %s ###", n+1, test.name)

		for i, input := range test.input {
			output := sampler.SampleEvent(input)
			assert.Equal(t, test.output[i].Type, output.Type)
			assert.Equal(t, test.output[i].Key, output.Key)
		}
	}
}
