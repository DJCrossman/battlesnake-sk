const { moveAsCoord, offBoard, onBody } = require('./move')

module.exports = class Main {
  instance
  firstMove

  constructor(state) {
    this.history = []
  }

  async handleMove (state) { // return string
    const head = state.you.body[0];
    const moves = ['right', 'up', 'down', 'left'].sort(() => Math.random() - 0.5)
    for (const move of moves) {
      const coord = moveAsCoord(move, head);
      if (!offBoard(state, coord) &&
        !onBody(state.you.body, coord)) {
        return move;
      }
    }
    return 'up';
  }

  async handleEnd (state) {

  }

}