package widgets

import (
	"github.com/ambientsound/pms/index"
	"github.com/ambientsound/pms/songlist"
	"github.com/ambientsound/pms/version"

	"github.com/gdamore/tcell"
	"github.com/gdamore/tcell/views"
)

type UI struct {
	// UI elements
	App           *views.Application
	Layout        *views.BoxLayout
	Topbar        *views.TextBar
	Columnheaders *ColumnheadersWidget
	Multibar      *MultibarWidget
	Songlist      *SongListWidget

	// Data resources
	Index           *index.Index
	defaultSongList *songlist.SongList

	// Styles
	styleTopbar      tcell.Style
	styleTopbarTitle tcell.Style

	// TCell
	view views.View
	views.WidgetWatchers
}

func NewUI() *UI {
	ui := &UI{}

	ui.App = &views.Application{}

	ui.Topbar = views.NewTextBar()
	ui.Columnheaders = NewColumnheadersWidget()
	ui.Multibar = NewMultibarWidget()
	ui.Songlist = NewSongListWidget()

	ui.Multibar.Watch(ui)
	ui.Songlist.Watch(ui)

	ui.styleTopbar = tcell.StyleDefault.Background(tcell.ColorBlue).Foreground(tcell.ColorWhite)
	ui.styleTopbarTitle = tcell.StyleDefault.Foreground(tcell.ColorWhite)

	ui.Topbar.SetStyle(ui.styleTopbar)
	ui.Topbar.SetLeft(version.ShortName(), ui.styleTopbar)
	ui.Topbar.SetRight(version.Version(), ui.styleTopbar)

	ui.Multibar.SetDefaultText("Type to search.")

	ui.CreateLayout()
	ui.App.SetRootWidget(ui)

	return ui
}

func (ui *UI) CreateLayout() {
	ui.Layout = views.NewBoxLayout(views.Vertical)
	ui.Layout.AddWidget(ui.Topbar, 0)
	ui.Layout.AddWidget(ui.Columnheaders, 0)
	ui.Layout.AddWidget(ui.Songlist, 2)
	ui.Layout.AddWidget(ui.Multibar, 0)
	ui.Layout.SetView(ui.view)
}

func (ui *UI) SetIndex(i *index.Index) {
	ui.Index = i
}

func (ui *UI) SetDefaultSonglist(s *songlist.SongList) {
	ui.defaultSongList = s
}

func (ui *UI) Start() {
	ui.App.Start()
}

func (ui *UI) Wait() error {
	return ui.App.Wait()
}

func (ui *UI) Quit() {
	ui.App.Quit()
}

func (ui *UI) Draw() {
	ui.Layout.Draw()
}

func (ui *UI) Resize() {
	ui.CreateLayout()
	ui.Layout.Resize()
	ui.PostEventWidgetResize(ui)
}

func (ui *UI) SetView(v views.View) {
	ui.view = v
	ui.Layout.SetView(v)
}

func (ui *UI) Size() (int, int) {
	return ui.view.Size()
}

func (ui *UI) HandleEvent(ev tcell.Event) bool {
	switch ev := ev.(type) {

	case *tcell.EventKey:
		switch ev.Key() {
		case tcell.KeyCtrlC:
			fallthrough
		case tcell.KeyCtrlD:
			ui.App.Quit()
			return true
		case tcell.KeyCtrlL:
			ui.App.Refresh()
			return true
		}

	case *EventListChanged:
		ui.App.Update()
		ui.Topbar.SetCenter(" "+ui.Songlist.Name()+" ", ui.styleTopbarTitle)
		ui.Columnheaders.SetColumns(ui.Songlist.Columns())
		return true

	case *EventInputChanged:
		term := ui.Multibar.GetRuneString()
		ui.runIndexSearch(term)
		return true

	case *EventScroll:
		ui.refreshPositionReadout()
		return true

	}

	if ui.Layout.HandleEvent(ev) {
		return true
	}

	return false
}

func (ui *UI) refreshPositionReadout() {
	str := ui.Songlist.PositionReadout()
	ui.Multibar.SetRight(str, tcell.StyleDefault)
}

func (ui *UI) runIndexSearch(term string) {
	if ui.Index == nil {
		return
	}
	if len(term) == 0 {
		ui.Songlist.SetCursor(0)
		ui.Songlist.SetSongList(ui.defaultSongList)
		return
	}
	if len(term) == 1 {
		return
	}
	results, err := ui.Index.Search(term)
	if err == nil {
		ui.Songlist.SetCursor(0)
		ui.Songlist.SetSongList(results)
		return
	}
}