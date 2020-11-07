const { moveAsCoord, offBoard, withinSet } = require('./move')

module.exports = class Main {
  instance
  firstMove

  constructor(state) {
    this.history = []
  }

  // number
  async calculateWeight(state, coord, weight) {
    const options = ['right', 'up', 'down', 'left'].sort(() => Math.random() - 0.5)
    if (weight < (1 / (state.board.height * 10))) {
      return weight
    }
    let moves
    if (withinSet(state.board.food, coord)) {
      moves = await Promise.all(
        options.map((move) =>
          this.calculateMove(
            {
              ...state,
              you: { ...state.you, body: [coord, ...state.you.body] },
            },
            move,
            weight / 5
          )
        )
      )
    } else {
      moves = await Promise.all(
        options.map((move) =>
          this.calculateMove(
            {
              ...state,
              you: {
                ...state.you,
                body: [
                  coord,
                  ...[...state.you.body].slice(0, state.you.body.length - 1),
                ],
              },
            },
            move,
            weight / 10
          )
        )
      )
    }
    const projectedWeight = moves.reduce((a, b) => a + b.weight, 0) / moves.length
    console.log(`${state.turn} - ${projectedWeight}`)
    return projectedWeight + (weight / 5)
  }

  // return { move, weight }
  async calculateMove(state, move, weight) {
    const head = state.you.head
    const coord = moveAsCoord(move, head)
    if (!offBoard(state, coord) && !withinSet(state.you.body, coord)) {
      const _weight = await this.calculateWeight(state, coord, weight)
      return { move, weight: _weight }
    }
    return { move, weight: 0 }
  }

  // return { move, weight }
  async findMove(state, weight) {
    const options = ['right', 'up', 'down', 'left'].sort(() => Math.random() - 0.5)
    const moves = await Promise.all(
      options.map((move) => this.calculateMove(state, move, weight))
    )
    console.log(`${state.turn} - ${weight} - ${JSON.stringify(state.you.head)}`, JSON.stringify(moves))
    return moves.reduce((a, b) => (a.weight >= b.weight ? a : b), {
      move: 'up',
      weight: 0,
    })
  }

  // return string
  async handleMove(state) {
    return (await this.findMove(state, 1)).move
  }

  async handleEnd(state) {}
}
